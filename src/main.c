#if !defined(CRT_LINKED) && !defined(NO_CRT_LINKED)
  #define CRT_LINKED
#endif
#define SUB_CONSOLE
#include <cm_entry.h>
#include <cm_error_handling.c>
#include <cm_io.c>
#include <cm_memory.c>
#include <cm_string.c>
#include "cm_hid.c"
#include "sdeck_icons.h"

#pragma warning(push, 0)
#include <windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <setupapi.h>
#if defined(CRT_LINKED)
  #include <stdio.h>
#endif
#pragma warning(pop)

#pragma comment(lib, "CfgMgr32.lib")
#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "hid.lib")

/* NOTE: Taken from elgato's repo */
#define VID_ELGATO              0x0fd9
#define PID_SDECK_ORIGINAL      0x0060
#define PID_SDECK_ORIGINAL_19   0x006d
#define PID_SDECK_MK2_21        0x0080
#define PID_SDECK_MK2_SCISSOR   0x0080 /* (2023) */
#define PID_SDECK_MINI          0x0063
#define PID_SDECK_MINI_22       0x0090
#define PID_SDECK_NEO           0x009a
#define PID_SDECK_XL            0x006c
#define PID_SDECK_XL_22         0x008f
#define PID_SDECK_PEDAL         0x0086
#define PID_SDECK_PLUS          0x0084 /* (Wave deck) */

global u16 g_elgato_pids[] = { 
  PID_SDECK_ORIGINAL, PID_SDECK_ORIGINAL_19, PID_SDECK_MINI, 
  PID_SDECK_NEO, PID_SDECK_XL, PID_SDECK_XL_22, PID_SDECK_MK2_21,
  PID_SDECK_PEDAL, PID_SDECK_MINI_22, PID_SDECK_PLUS, PID_SDECK_MK2_SCISSOR,
};

#pragma warning(disable : 4820)
/* NOTE: This is currently based off StreamDeckXL's values */
typedef struct StreamDeck
{
  u8         rows;                 // 4
  u8         cols;                 // 8
  u8         total;                // rows * cols
  u16        pxl_w;                // 96
  u16        pxl_h;                // 96
  u8         img_rpt_header_len;   // 8
  u32        img_rpt_payload_len;  // img_rpt - img_rpt_header
  u32        img_rpt_len;          // 1024
  char*      img_format;           // "JPEG"
  u8         key_rotation;         // 0
  u8         *blank_key;
  u32        key_states;           // 32 packed keys
  HidDevice* hid;
}StreamDeck;
#pragma warning(default : 4820)

HidDevice*
sdk_get_hid_device(void)
{
  HidDeviceInfo *info = NULL;
  for (u32 i = 0; i < countof(g_elgato_pids); i++)
  {
    info = hid_enumerate(VID_ELGATO, g_elgato_pids[i]);
    if (info) break;
  }
  if (!info) {printf("No streamdeck found.\n"); return NULL;}
  else printf("Found streamdeck !\n");

  HidDevice* deck = hid_get_device(info);
  if (!deck) hid_close_info(info);
  return deck;
}

i64
sdk_get_report(StreamDeck* sdk)
{
  return hid_get_report(sdk->hid->h_dev, sdk->hid->feature, IOCTL_HID_GET_FEATURE);
}

i64
sdk_set_brightness(StreamDeck* sdk, u8 percent)
{
  u8        buffer[3];
  i64       written = 0;
  HidReport report  = { .size = 3};

  percent    = (percent >= 100) ? 100 : percent;
  buffer[0]  = 0x03;
  buffer[1]  = 0x08;
  buffer[2]  = percent;
  report.buf = buffer;
  written    = hid_send_report(sdk->hid, report, HID_SEND_FEATURE);
  if (written == -1) printf("sdk_set_brightness failed\n");
  return written;
}

#define SDK_IMAGE_SIZE 1024
/* NOTE: It seems we need to rotate 90 degrees the image */
#pragma warning(disable : 4333)
i64
sdk_set_key_image(StreamDeck* sdk, u8 key, u8 *image, u32 image_size)
{
  u8        buffer[SDK_IMAGE_SIZE];
  i64       written = 0;
  HidReport report  = { .size = SDK_IMAGE_SIZE};

  if (key >= sdk->total)
  {
    written = -2;
    goto failure;
  }

  report.buf = buffer;
  buffer[0]  = 0x02; /* NOTE: Report ID */
  buffer[1]  = 0x07; /* NOTE: Command ID */
  buffer[2]  = key;

  i64 left        = image_size;
  u32 loops       = 0;
  u64 to_copy     = 0;
  u64 track_copy  = 0;
  u32 header_len  = sdk->img_rpt_header_len;
  u32 payload_len = sdk->img_rpt_payload_len;
  while (left > 0)
  {
    /* NOTE: Ensures we properly reset the buffer */
    memset(buffer + 3, 0, payload_len + 5);
    to_copy    = (left >= payload_len) ? payload_len : left;
    track_copy = loops * payload_len;

    buffer[3] = ((i64) to_copy == left);
    buffer[4] = (u8)(to_copy & 0xff);
    buffer[5] = (u8)(to_copy >> header_len);
    buffer[6] = (u8)(loops & 0xFF);
    buffer[7] = (u8)(loops >> header_len);

    memcpy(buffer + 8, image + track_copy, to_copy);
    left   -= to_copy;
    written = hid_write(sdk->hid, report);
    loops++;
    if (written == -1) goto failure;
  }
failure:
  switch (written)
  {
    case -1: printf("sdk_set_key_image failed\n"); break;
    case -2: printf("Invalid key\n"); break;
    default: break;
  }
  return written;
}
#pragma warning(default : 4333)

/*
 * TODO:
 *       [_]: This should be done on a separate thread
 *       [_]: Resize images to fit streamdeck's expected output
 *       [_]: GIF's
 */
i64
sdk_set_key_image_path(StreamDeck* sdk, u8 key, char* path)
{
  cmFile file = {0};
  if (file_exist_open_map_ro(path, &file) != CM_OK)
  {
    report_error("file_exist_open_map_ro");
    return -1;
  }
  u8* image      = file.buffer.view;
  u32 image_size = (u32) file.buffer.size;
  i64 written    = sdk_set_key_image(sdk, key, image, image_size);
  file_close(&file);
  return written;
}

i64
sdk_reset_key_stream(StreamDeck* sdk)
{
  u8        buffer[SDK_IMAGE_SIZE];
  i64       written = 0;
  HidReport report  = { .size = SDK_IMAGE_SIZE};

  memset(buffer, 0, SDK_IMAGE_SIZE);
  report.buf    = buffer;
  report.buf[0] = 0x02;
  written = hid_write(sdk->hid, report);
  return written;
}

i64
sdk_reset(StreamDeck* sdk)
{
  u8        buffer[2000];
  HidReport report  = { .size = 2};

  buffer[0]   = 0x03;
  buffer[1]   = 0x02;
  report.buf  = buffer;
  i64 written = hid_send_report(sdk->hid, report, HID_SEND_FEATURE);
  return written;
}

#define SDK_KEY_HEADER      27
#define SDK_KEY_DATA        512
#define SDK_KEY_INPUT_SIZE  (SDK_KEY_HEADER + SDK_KEY_DATA)

void
print_pressed(StreamDeck *sdk)
{
  u32 key_states = sdk->key_states;
  for (int i = 0; i < 32; i++)
  {
    if ((key_states >> i) & 1)
    {
      printf("Key %u is pressed\n", i);
    }
  }
}

/*
 * TODO:
 *       [_]: We should not be waiting on the main thread with this function.
 *            As for now read_timeout_ms is set to 1'000 ms to prevent hanging.
 *       [_]: Save the current key state to recognize when long pressing.
 */
i64
sdk_read_input(StreamDeck* sdk)
{
  u8        buffer[SDK_KEY_INPUT_SIZE];
  i64       read = 0;
  HidReport data = {.buf = buffer, .size = SDK_KEY_INPUT_SIZE,};
  memset(buffer, 0, SDK_KEY_INPUT_SIZE);

  read = hid_read(sdk->hid, data);
  if (read == -1) console_debug("sdk_read_input failed")
  else
  {
    /* NOTE: Skip the header and get the key states. */
    u32 max = 4 + sdk->total;
    for (u32 i = 4, j = 0; i < max ; i++, j++)
    {
      if (data.buf[i]) sdk->key_states |= (1 << j);
    }
  }
  return read;
}

ENTRY
{
  i64 read       = -1;
  i64 written    = -1;
  StreamDeck sdk = {
    .rows                = 4,
    .cols                = 8,
    .total               = 8 * 4,
    .pxl_w               = 96,
    .pxl_h               = 96,
    .img_rpt_header_len  = 8,
    .img_rpt_len         = 1024,
    .img_rpt_payload_len = 1024 - 8,
    .img_format          = "JPEG",
    .key_rotation        = 0,
    .blank_key           = blank_key_img,
  };
  sdk.hid = sdk_get_hid_device();
  if (sdk.hid)
  {
    if (sdk_read_input(&sdk) != -1) print_pressed(&sdk);
    /*
     * sdk_reset(&sdk);
     * sdk_reset_key_stream(&sdk);
     * sdk_set_brightness(&sdk, 100);
     * sdk_set_key_image(&sdk, 0x00, blank_key_img, countof(blank_key_img));
     */
  }
  hid_close_device(sdk.hid);
  printf("Exiting..\n");
  RETURN_FROM_MAIN(EXIT_SUCCESS);
}
