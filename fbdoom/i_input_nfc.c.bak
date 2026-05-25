#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libserialport.h>
#include <assert.h>
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



typedef struct {
  char tag_id[7];
  char doom_keycode;
  char is_end;
} tag_info;

tag_info all_tags[] = {
  { .tag_id = { 0x04, 0x04, 0xcb, 0x65, 0x5f, 0x61, 0x80 }, .doom_keycode = KEY_UPARROW },
  { .tag_id = { 0x04, 0x51, 0xcd, 0x65, 0x5f, 0x61, 0x80 }, .doom_keycode = KEY_DOWNARROW },
  { .tag_id = { 0x04, 0x5c, 0xbe, 0x54, 0x6f, 0x61, 0x81 }, .doom_keycode = KEY_LEFTARROW },
  { .tag_id = { 0x04, 0x79, 0x03, 0x54, 0x6F, 0x61, 0x80 }, .doom_keycode = KEY_RIGHTARROW },
  { .tag_id = { 0x04, 0x99, 0x66, 0x63, 0x5F, 0x61, 0x80 }, .doom_keycode = KEY_FIRE },
  { .tag_id = { 0x04, 0x4D, 0xD6, 0x64, 0x5F, 0x61, 0x80 }, .doom_keycode = 0 },
  { .tag_id = { 0x04, 0x42, 0xC4, 0x4F, 0x6F, 0x61, 0x80 }, .doom_keycode = KEY_USE },
  { .tag_id = { 0x04, 0x5B, 0xE1, 0x34, 0x4F, 0x61, 0x80 }, .doom_keycode = KEY_ENTER },
  { .is_end = 1 },
};

#define HEADER_LENGTH (7)

int vanilla_keyboard_mapping = 1;

struct sp_port *nfc_port = NULL;

char current_message[65536 + 4 + 8] = { 0 };

char prev_key = 0;

int offset = 0;

void print_hex(char* buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    fprintf(stderr, "%02hhx", buf[i]);
  }
  fprintf(stderr, "\n");
}

/* Helper function for error handling. */
int check(enum sp_return result)
{
        /* For this example we'll just exit on any error by calling abort(). */
        char *error_message;
        switch (result) {
        case SP_ERR_ARG:
                printf("Error: Invalid argument.\n");
                abort();
        case SP_ERR_FAIL:
                error_message = sp_last_error_message();
                printf("Error: Failed: %s\n", error_message);
                sp_free_error_message(error_message);
                abort();
        case SP_ERR_SUPP:
                printf("Error: Not supported.\n");
                abort();
        case SP_ERR_MEM:
                printf("Error: Couldn't allocate memory.\n");
                abort();
        case SP_OK:
        default:
                return result;
        }
}

// returns whether we should keep trying to read
boolean nfc_read(void)
{
  static_assert(sizeof(ipp_header) == 7);
  if (nfc_port == NULL)
    return false;

  if (offset < HEADER_LENGTH) {
    // fprintf(stderr, "trying to read header\n");
    int ret = check(sp_nonblocking_read(nfc_port, current_message + offset, HEADER_LENGTH - offset));
    if (ret == 0)
      return false;
    // fprintf(stderr, "serial: read %d bytes of header\n", ret);
    // print_hex(current_message + offset, ret);
    if (current_message[0] != 0xbc) {
      // fprintf(stderr, "got non-ipp message\n");
      offset = 0;
      return true;
    }
    offset += ret;
  }
  if (offset >= HEADER_LENGTH) {
    ipp_header head;

    memcpy(&head, current_message, HEADER_LENGTH);


    int data_length = (head.data_length_msb << 8) | head.data_length_lsb;

    int full_length = HEADER_LENGTH + data_length;

    int ret = check(sp_nonblocking_read(nfc_port, current_message + offset, full_length - offset));
    if (ret == 0)
      return false;
    // fprintf(stderr, "serial: read %d bytes of data\n", ret);
    // print_hex(current_message + offset, ret);
    offset += ret;
    // fprintf(stderr, "data_length_msb is %02hhx\n", head.data_length_msb);
    // fprintf(stderr, "data_length_lsb is %02hhx\n", head.data_length_lsb);
    // fprintf(stderr, "offset is %d\n", offset);
    // fprintf(stderr, "length is %d\n", full_length);

    if (offset >= full_length) {
      // fprintf(stderr, "Got ipp message\n");
      offset = 0;
      // 0xbe = PICC Read
      if (head.message_type == 0xbe) {
        if (data_length != 21) {
          fprintf(stderr, "dunno how to read length %d", data_length);
          return true;
        }
        char tag_id[7];
        memcpy(tag_id, current_message + HEADER_LENGTH + 11, 7);
        fprintf(stderr, "read tag uid ");
        print_hex(tag_id, 7);
        for (int i=0; all_tags[i].is_end == 0; i++) {
          tag_info this_tag = all_tags[i];
          if (memcmp(this_tag.tag_id, tag_id, 7) == 0) {
            event_t ev;
            fprintf(stderr, "found tag with keycode %d\n", (int)this_tag.doom_keycode);
            if (prev_key != 0) {
              ev.type = ev_keyup;
              ev.data1 = prev_key;
              ev.data2 = 0;
              D_PostEvent(&ev);
            }
            char this_keycode = this_tag.doom_keycode;
            prev_key = this_keycode;
            if (this_keycode != 0) {
              ev.type = ev_keydown;
              ev.data1 = this_tag.doom_keycode;
              ev.data2 = 0;
              D_PostEvent(&ev);
            }
            break;
          }
        }
      }
      return true;
    }
  }
  return true;
}

void I_GetEvent(void)
{
  // fprintf(stderr, "I_GetEvent\n");
  while (nfc_read()) {}
}

void I_InitInput(void)
{
  fprintf(stderr, "get port\n");
  check(sp_get_port_by_name("/dev/ttyS3", &nfc_port));
  fprintf(stderr, "open port\n");
  check(sp_open(nfc_port, SP_MODE_READ));
  fprintf(stderr, "setting stuffs\n");
  check(sp_set_baudrate(nfc_port, 115200));
  check(sp_set_bits(nfc_port, 8));
  check(sp_set_parity(nfc_port, SP_PARITY_NONE));
  check(sp_set_stopbits(nfc_port, 1));
  check(sp_set_flowcontrol(nfc_port, SP_FLOWCONTROL_NONE));
}

