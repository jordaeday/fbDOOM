#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "config.h"
#include "deh_str.h"
#include "doomtype.h"
#include "doomkeys.h"
#include "i_joystick.h"
#include "i_system.h"
#include "i_swap.h"
#include "i_timer.h"
#include "i_video.h"
#include "i_scale.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "tables.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

typedef struct {
  uint8_t magic;
  uint8_t sequence_number;
  uint8_t unk;
  uint8_t message_type;
  uint8_t data_length_msb;
  uint8_t data_length_lsb;
  uint8_t crc;
} ipp_header;

#define HEADER_LENGTH 7

typedef struct {
  char tag_id[7];
  char doom_keycode;
  char is_end;
} tag_info;

tag_info all_tags[] = {
  { .tag_id = { 0x04, 0x2C, 0x94, 0xE2, 0x85, 0x21, 0x90 }, .doom_keycode = KEY_UPARROW },
  { .tag_id = { 0x04, 0x7A, 0xE9, 0xE2, 0x85, 0x21, 0x90 }, .doom_keycode = KEY_DOWNARROW },
  { .tag_id = { 0x04, 0x70, 0x6A, 0xB2, 0x82, 0x21, 0x90 }, .doom_keycode = KEY_LEFTARROW },
  { .tag_id = { 0x04, 0xB8, 0x12, 0xE2, 0x85, 0x21, 0x90 }, .doom_keycode = KEY_RIGHTARROW },
  { .tag_id = { 0x04, 0x32, 0x55, 0xB2, 0x82, 0x21, 0x91 }, .doom_keycode = KEY_FIRE },
  { .tag_id = { 0x04, 0x49, 0x9B, 0xE2, 0x85, 0x21, 0x90 }, .doom_keycode = 0 },
  { .tag_id = { 0x04, 0x3E, 0x77, 0xE2, 0x85, 0x21, 0x90 }, .doom_keycode = KEY_USE },
  { .tag_id = { 0x04, 0x10, 0x12, 0xE2, 0x85, 0x21, 0x91 }, .doom_keycode = KEY_ENTER },
  { .is_end = 1 },
};

int vanilla_keyboard_mapping = 1;

// fd for the NFC serial port; -1 means not open / disabled
int nfc_port = -1;

static char current_message[65536 + 4];
static int offset = 0;
int prev_key = 0;

void print_hex(char* buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    fprintf(stderr, "%02hhx", buf[i]);
  }
  fprintf(stderr, "\n");
}

// Non-blocking read wrapper. Returns bytes read, or 0 if no data available.
static int serial_read(int fd, void *buf, size_t count) {
  int ret = read(fd, buf, count);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;
    fprintf(stderr, "NFC serial read error: %s\n", strerror(errno));
    return 0;
  }
  return ret;
}

// Returns true if there may be more data to process.
static boolean nfc_read(void)
{
  if (nfc_port < 0)
    return false;

  // --- Read header ---
  if (offset < HEADER_LENGTH) {
    int ret = serial_read(nfc_port, current_message + offset, HEADER_LENGTH - offset);
    if (ret == 0)
      return false;

    if (offset == 0) {
      // Scan forward to the 0xBC magic byte
      int i = 0;
      while (i < ret && (unsigned char)current_message[i] != 0xbc)
        i++;
      if (i > 0) {
        memmove(current_message, current_message + i, ret - i);
        ret -= i;
      }
      if (ret == 0)
        return true;
    }
    offset += ret;
    return true;
  }

  // --- Parse header ---
  ipp_header head;
  memcpy(&head, current_message, HEADER_LENGTH);
  int data_length = (head.data_length_msb << 8) | head.data_length_lsb;
  // Types >= 0x80 have a 4-byte CRC32 appended after the payload
  int full_length = HEADER_LENGTH + data_length + (head.message_type >= 0x80 ? 4 : 0);

  // --- Read body ---
  if (offset < full_length) {
    int ret = serial_read(nfc_port, current_message + offset, full_length - offset);
    if (ret == 0)
      return false;
    offset += ret;
    return true;
  }

  // --- Full packet received ---
  offset = 0;

  // Discard heartbeats and anything that isn't a card event
  if (head.message_type != 0xd5)
    return true;

  unsigned char subcmd = (unsigned char)current_message[HEADER_LENGTH];

  if (subcmd == 0x01) {
    // Card removed — release whatever key was held
    if (prev_key != 0) {
      event_t ev;
      ev.type = ev_keyup;
      ev.data1 = prev_key;
      ev.data2 = 0;
      D_PostEvent(&ev);
      prev_key = 0;
    }
    return true;
  }

  if (subcmd != 0x00 || data_length < 9)
    return true;

  // Card entry: UID is at payload offset +2
  char tag_id[7];
  memcpy(tag_id, current_message + HEADER_LENGTH + 2, 7);
  fprintf(stderr, "card uid: ");
  print_hex(tag_id, 7);

  int matched = 0;
  for (int i = 0; all_tags[i].is_end == 0; i++) {
    if (memcmp(all_tags[i].tag_id, tag_id, 7) == 0) {
      int this_keycode = (unsigned char)all_tags[i].doom_keycode;
      fprintf(stderr, "matched keycode %d\n", this_keycode);
      event_t ev;
      if (prev_key != 0 && prev_key != this_keycode) {
        ev.type = ev_keyup;
        ev.data1 = prev_key;
        ev.data2 = 0;
        D_PostEvent(&ev);
      }
      prev_key = this_keycode;
      if (this_keycode != 0) {
        ev.type = ev_keydown;
        ev.data1 = this_keycode;
        ev.data2 = 0;
        D_PostEvent(&ev);
      }
      matched = 1;
      break;
    }
  }
  if (!matched)
    fprintf(stderr, "no uid match\n");

  return true;
}

void I_GetEvent(void)
{
  while (nfc_read()) {}
}

void I_InitInput(void)
{
  fprintf(stderr, "opening NFC serial port\n");

  int fd = open("/dev/ttymxc3", O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "Failed to open /dev/ttymxc3: %s — NFC input disabled\n", strerror(errno));
    nfc_port = -1;
    return;
  }

  struct termios tty;
  if (tcgetattr(fd, &tty) < 0) {
    fprintf(stderr, "tcgetattr failed: %s — NFC input disabled\n", strerror(errno));
    close(fd);
    nfc_port = -1;
    return;
  }

  // 115200 baud
  cfsetispeed(&tty, B115200);
  cfsetospeed(&tty, B115200);

  // 8N1, no flow control, raw mode
  tty.c_cflag &= ~PARENB;           // no parity
  tty.c_cflag &= ~CSTOPB;           // 1 stop bit
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;               // 8 data bits
  tty.c_cflag &= ~CRTSCTS;          // no hardware flow control
  tty.c_cflag |= CREAD | CLOCAL;    // enable receiver, ignore modem lines

  tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);  // raw input
  tty.c_iflag &= ~(IXON | IXOFF | IXANY);          // no software flow control
  tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
  tty.c_oflag &= ~OPOST;            // raw output

  tty.c_cc[VMIN]  = 0;             // non-blocking: return immediately
  tty.c_cc[VTIME] = 0;

  if (tcsetattr(fd, TCSANOW, &tty) < 0) {
    fprintf(stderr, "tcsetattr failed: %s — NFC input disabled\n", strerror(errno));
    close(fd);
    nfc_port = -1;
    return;
  }

  fprintf(stderr, "NFC serial port ready\n");
  nfc_port = fd;
}
