#include <string>
#include <stdio.h>
#include <stdint.h>
#include <Windows.h>
#include <SetupAPI.h>
#include <hidsdi.h>
#include <cfgmgr32.h>
#include <tchar.h>

/* compile from the command line:
*  "cl MS2109_stereo_fix.cpp hid.lib setupapi.lib shell32.lib"
*/

// change these if your Macrosilicon 2109 doesn't use the standard VID/PIDs
#define MS2109_VID 0x534D
#define MS2109_PID 0x2109

// some interesting address in the MS2109 XDATA
// the settings are signed 16-bit values
#define BRIGHTNESS                  0xC6A0 // default: -11
#define CONTRAST                    0xC6A2 // default: 148
#define HUE                         0xC6A4 // default: 0
#define SATURATION                  0xC6A6 // default: 180

#define ADDR_INPUT_WIDTH            0xC6AF
#define ADDR_INPUT_HEIGHT           0xC6B1
#define ADDR_INPUT_FPS              0xC6B5
#define ADDR_INPUT_PIXELCLK         0xC73C  // C6B3?
#define ADDR_GPIO                   0xDF00
#define ADDR_SPDIFOUT               0xDF01

#define ADDR_HDMI_CONNECTION_STATUS 0xFA8C
#define ADDR_BRIGHTNESS             0xFE90
#define ADDR_CONTRAST               0xFE91
#define ADDR_SATURATION             0xFE92
#define ADDR_HUE                    0xFE93

/* I don't have a device with a 24C32/23C64 EEPROM but in theory they should work...
*  The difference is data byte 4 in the feature report must be 1 instead of 0 to use
*  16-bit I2C addressing instead of what the smaller EEPROMs use (a single address
*  byte with the upper 3 address bits packed into the I2C device 3 lower bits)
*/
uint16_t max_eeprom_address = 0x800;

HANDLE ms2109 = INVALID_HANDLE_VALUE;

/* instance ID of the USB composite device, passed to pnputil to remove the old driver.
 * This is necessary because windows is too stupid to realize the device descriptor has
 * changed and will keep trying to use the old format (it caches the descriptors in the
 * registry), which will make directshow fail with vague/mysterious errors.
 */
TCHAR* ms2109_instance = NULL;

/* EEPROM layout (multibyte values are big-endian):
*  Bytes 0-1: 0xA5 0x5A or 0x96 0x69
*  Bytes 2-3: size of the code in bytes, starting at offset 0x30 that gets loaded to 0xCC00.
*  Byte 4: seems to be a bitmask specifying which utility functions the EEPROM code will implement:
*   Bit 0 = "Patch_Common" offset 0xCC00, Bit 1 = "USB_cmd" offset 0xCC10?, Bit 2 = "USB_int" 0xCC20,
*   Bit 3 = "Timer_int" (some firmwares include this, most don't), Bit 4 = "VSync_int",
*   Bit 5 = "UART_int"
*  Byte 5: Same as previous byte but for HDMI functions:
*   Bit 0: "hdmi rx hotplug svc replace"
*   Bit 1: "hdmi rx pl svc replace"
*   Bit 2: "hdmi rx mdt svc replace"
*   Bit 3: "hdmi rx packet svc replace"
*   Bit 4: "hdmi rx edid svc replace" this is the only one I've seem implemented, it pokes the EDID's xdata address into a register
*   Bit 5: "hdmi rx hdcp svc replace"
*  Bytes 6-7: replacement VID or 0xFFFF to use the default
*  Bytes 8-9: replacement PID or 0xFFFF to use the default
*  Byte 10: more bitflags, bit 0 seems to be something to do with HID (enlarges wMaxPacket for endpoint)
*  Byte 11: ??
*  Bytes 12-15: Version DWORD. Usually a date is here (YY-YY-MM-DD).
*  Bytes 16-31: Length-prefixed string for the video interface, else filled with 0xFF. There are dodgy firmwares that put "USB3.0 ..." here to try to convince the user that it's
*  a USB 3.0 device (spoiler: it's not).
*  Bytes 32-47: Length-prefixed string for the audio interface, else filled with 0xFF
*  Bytes 30-N: 8051 code that will be mapped to 0xCC00 in CODE and XDATA. (Actually the header gets mapped as well starting at 0xCBD0, at least in XDATA.)
*  Bytes N-N+1: 16-bit checksum of bytes 2-0x2F.
*  Bytes N+2-N+3: 16-bit checksum of bytes 0x30-N.
*/

/* Patch description:
*  Typically the very first code sequence in the EEPROM (which is CODE address 0xCC00) looks like this:
*   MOV DPTR, @XXXX
*   MOV A, R7
*   MOVX @DPTR, A
*  We put new code at the end of the EEPROM and overwrite the "MOV DPTR..." instruction with a call to it. When R7==2 the patch modifies the audio format descriptor (which lives at a
*  fixed offset in XDATA).
*  Alternatively it would be possible to use the USB_int hook to check if bit 2 of byte [33] is set. If it is set, clear it, call code@6069, then patch the audio format descriptor.
*  6069 seems to be the function that loads the configuration descriptor into XDATA from CODE.
*  Not all firmwares hook USB_int though so I went with the more "universal" method.
*
*  Note that this simply makes the host PC interpret the audio as stereo instead of mono; the MS2109 has a bug that causes it to emit one extra sample at the beginning
*  which causes:
*   a) the left and right channels to be swapped (reverse-stereo)
*   b) out of phase by one sample
*/

// This is just junk to get the USB device's instance ID to pass to pnputil. It's not essential for the patching process.
static void get_device_instance_name(DEVINST hid_child) {
  DEVINST hid_intf;
  if (CM_Get_Parent(&hid_intf, hid_child, 0) != CR_SUCCESS)
    return;

  // hid_intf is the HID interface on the USB composite device; we want the instance id for the composite device, so go up again
  DEVINST composite;
  if (CM_Get_Parent(&composite, hid_intf, 0) != CR_SUCCESS)
    return;

  ULONG len;
  if (CM_Get_Device_ID_Size(&len, composite, 0) != CR_SUCCESS)
    return;

  ms2109_instance = (TCHAR*)malloc(sizeof(TCHAR) * (len + 1));
  if (ms2109_instance != NULL) {
    if (CM_Get_Device_ID(composite, ms2109_instance, len + 1, 0) != CR_SUCCESS || _tcsncmp(ms2109_instance, TEXT("USB\\VID_534D&PID_2109\\"), 22)!=0) {
      free(ms2109_instance);
      ms2109_instance = NULL;
    }
  }
}

// get a handle for the HID interface on the MS2109. We need this to get/set feature reports.
static void find_device(void) {
  GUID guid;
  HDEVINFO info;

  HidD_GetHidGuid(&guid);
  info = SetupDiGetClassDevs(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (info == INVALID_HANDLE_VALUE)
    return;

  for (DWORD index = 0; ;index++) {
    SP_DEVICE_INTERFACE_DATA iface;
    SP_DEVINFO_DATA devinfo;
    DWORD required_size = 0;

    iface.cbSize = sizeof(iface);
    if (!SetupDiEnumDeviceInterfaces(info, NULL, &guid, index, &iface))
      break;

    SetupDiGetInterfaceDeviceDetail(info, &iface, NULL, 0, &required_size, NULL);
    if (required_size == 0) continue;
    SP_DEVICE_INTERFACE_DETAIL_DATA* details = (SP_DEVICE_INTERFACE_DETAIL_DATA*)malloc(required_size);
    if (details == NULL) continue;

    memset(details, 0, required_size);
    details->cbSize = sizeof(*details);
    memset(&devinfo, 0, sizeof(devinfo));
    devinfo.cbSize = sizeof(devinfo);
    if (!SetupDiGetDeviceInterfaceDetail(info, &iface, details, required_size, NULL, &devinfo)) {
      free(details);
      continue;
    }

    HANDLE dev = CreateFile(details->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    free(details);
    if (dev == INVALID_HANDLE_VALUE)
      continue;

    HIDD_ATTRIBUTES attrib;
    attrib.Size = sizeof(attrib);
    if (HidD_GetAttributes(dev, &attrib) && attrib.VendorID==MS2109_VID && attrib.ProductID==MS2109_PID) {
      fprintf(stderr, "Found MS2109 device, VID %04X PID %04X bcdVersion %04X\n", attrib.VendorID, attrib.ProductID, attrib.VersionNumber);
      ms2109 = dev;
      get_device_instance_name((DEVINST)devinfo.DevInst);
      break;
    }

    CloseHandle(dev);
  }

  SetupDiDestroyDeviceInfoList(info);
}

struct feature_report {
  uint8_t report_id; // always 0
  uint8_t cmd; // E5 = read eeprom, E6 = write eeprom, B5 = read XDATA, B6 = write XDATA, C5/C6 = read/write ???
  uint8_t address_hi;
  uint8_t address_lo;
  uint8_t data[5];
};

template <class c>
static BOOLEAN read_eeprom(uint16_t address, c& val) {
  if (address >= max_eeprom_address) return false;
  feature_report rep = {};
  rep.report_id = 0;
  rep.cmd = 0xE5;
  rep.address_hi = (uint8_t)(address >> 8);
  rep.address_lo = (uint8_t)address;
  rep.data[4] = max_eeprom_address >> 12;
  if (HidD_SetFeature(ms2109, &rep, sizeof(rep))) {
    if (HidD_GetFeature(ms2109, &rep, sizeof(rep))) {
      val = 0;
      for (int i = 0; i < sizeof(c); i++) {
        val = (val << 8) | rep.data[i];
      }
      return true;
    }
    else fprintf(stderr, "Failed to read EEPROM @ %04X\n", address);
  }
  else fprintf(stderr, "Failed to set EEPROM read address %04X\n", address);
  return false;
}

static BOOLEAN read_eeprom_byte( uint16_t address, uint8_t& val) {
  return read_eeprom(address, val);
}

static BOOLEAN read_eeprom_word(uint16_t address, uint16_t& val) {
  return read_eeprom(address, val);
}

static BOOLEAN read_eeprom_dword(uint16_t address, uint32_t& val) {
  return read_eeprom(address, val);
}

static BOOLEAN write_eeprom_byte(uint16_t address, uint8_t src) {
  if (address >= max_eeprom_address) return false;
  feature_report rep = {};
  rep.report_id = 0;
  rep.cmd = 0xE6;
  rep.address_hi = (uint8_t)(address >> 8);
  rep.address_lo = (uint8_t)address;
  rep.data[0] = src;
  rep.data[4] = max_eeprom_address >> 12;
  if (HidD_SetFeature(ms2109, &rep, sizeof(rep))) {
    uint8_t d;
    if (!read_eeprom_byte(address, d))
      return false;
    if (d != src) {
      fprintf(stderr, "Failed to verify EEPROM @ %04X after writing (expected %02X actual %02X)\n", address, src, d);
      return false;
    }
    return true;
  }
  else fprintf(stderr, "Failed to write EEPROM @ %04X\n", address);
  return false;
}

static BOOLEAN write_eeprom_word(uint16_t address, uint16_t src) {
  if (!write_eeprom_byte(address, (uint8_t)(src >> 8)))
    return false;
  if (!write_eeprom_byte(address + 1, (uint8_t)src))
    return false;
  return true;
}

static BOOLEAN read_xdata_byte(uint16_t address, uint8_t& val) {
  feature_report rep = {};
  rep.report_id = 0;
  rep.cmd = 0xB5;
  rep.address_hi = (uint8_t)(address >> 8);
  rep.address_lo = (uint8_t)address;
  if (HidD_SetFeature(ms2109, &rep, sizeof(rep))) {
    if (HidD_GetFeature(ms2109, &rep, sizeof(rep))) {
      val = rep.data[0];
      return true;
    }
    else fprintf(stderr, "Failed to read XDATA @ %04X\n", address);
  }
  else fprintf(stderr, "Failed to set XDATA read address %04X\n", address);
  return false;
}

static BOOLEAN read_xdata_word(uint16_t address, uint16_t& val) {
  uint8_t d[2];
  if (read_xdata_byte(address, d[1]) && read_xdata_byte(address + 1, d[0])) {
    val = (d[1] << 8) | d[0];
    return true;
  }
  return false;
}

static BOOLEAN write_xdata_byte(uint16_t address, uint8_t val) {
  feature_report rep = {};
  rep.report_id = 0;
  rep.cmd = 0xB6;
  rep.address_hi = (uint8_t)(address >> 8);
  rep.address_lo = (uint8_t)address;
  rep.data[0] = val;
  if (HidD_SetFeature(ms2109, &rep, sizeof(rep))) {
    return true;
  }
  else fprintf(stderr, "Failed to write XDATA %04X\n", address);
  return false;
}

static BOOLEAN has_mono_descriptor(void) {
  uint8_t audio_format_channels;
  uint8_t audio_format_rate[3];

  if (!read_xdata_byte(0xC4C5, audio_format_channels)) {
    fprintf(stderr, "Failed to read audio format channels\n");
    return false;
  }
  if (!read_xdata_byte(0xC4C9, audio_format_rate[0]) || \
    !read_xdata_byte(0xC4CA, audio_format_rate[1]) || \
    !read_xdata_byte(0xC4CB, audio_format_rate[2])) {
    fprintf(stderr, "Failed to read audio format sampling rate\n");
    return false;
  }
  if (audio_format_channels != 1) {
    fprintf(stderr, "Audio format channels was not 1 (%d)\n", audio_format_channels);
    return false;
  }

  if (audio_format_rate[0] != 0x00 || audio_format_rate[1] != 0x77 || audio_format_rate[2] != 0x01) {
    fprintf(stderr, "Audio format sampling rate was not 96000 (%d)\n", (audio_format_rate[2] << 16) | (audio_format_rate[1] << 8) | audio_format_rate[0]);
    return false;
  }

  return true;
}

static BOOLEAN identify_eeprom() {
  uint16_t d;

  // check xdata where the EEPROM is mapped first, to figure out what type it is
  if (read_xdata_word(0xCBD0, d)) {
    if (d == 0xA55A)
      max_eeprom_address = 0x800;
    else if (d == 0x9669)
      max_eeprom_address = 0x1000;
  }

  if (read_eeprom_word(0, d)) {
    if (d == 0xA55A && max_eeprom_address == 0x800)
      return true;
    if (d == 0x9669 && max_eeprom_address == 0x1000)
      return true;
    // eeprom signature didn't match expected signature, try harder...
  }

  max_eeprom_address = 0x800;
  if (read_eeprom_word(0, d) && d == 0xA55A)
    return true;

  max_eeprom_address = 0x1000;
  if (read_eeprom_word(0, d) && d == 0x9669)
    return true;

  fprintf(stderr, "Failed to recognize EEPROM signature (%04X)\n", d);
  return false;
}

static BOOLEAN identify_ms2109(void) {
  uint8_t id[3] = {};

  if (read_xdata_byte(0xF800, id[0]) && \
    read_xdata_byte(0xF801, id[1]) && \
    read_xdata_byte(0xF802, id[2])) {
    if (id[0] == 0xA7 && id[1] == 0x10 && id[2] == 0x9A)
      return true;
  }

  fprintf(stderr, "Failed to identify MS2109 chip (%02X:%02X:%02X)\n", id[0], id[1], id[2]);
  return false;
}


static int attempt_patch(void) {
  uint16_t data_size;
  uint16_t hdr_sum;
  uint16_t data_sum;
  uint8_t audio_format_patch[] = {
    0xEF,             // mov A, R7
    0xB4, 0x02, 17  , // cjne A, #2, 1f    ; stage 2 = patch the audio format descriptor, else skip everything here
    0x90, 0xC4, 0xC5, // mov DPTR, #0xC4C5 ; audio format channels byte offset
    0xF0,             // movx @DPTR, A     ; set channels to 2
    0x90, 0xC4, 0xC9, // mov DPTR, #0xC4C9 ; audio format sampling rate byte offset
    0x74, 0x80,       // mov A, #0x80      ; 48000 & 0xFF
    0xF0,             // movx @DPTR,A
    0xA3,             // inc DPTR
    0x74, 0xBB,       // mov A, #0xBB      ; (48000 >> 8) & 0xFF
    0xF0,             // movx @DPTR,A
    0xA3,             // inc DPTR
    0xE4,             // clr A             ; 48000 >> 16
    0xF0,             // movx @DPTR,A      ; sampling rate = 48000, not 96000
    // 1:
    0x90, 0x00, 0x00, // mov DPTR, #XXXX   ; instruction that was overwritten with lcall to this patch
    0x22,             // ret
  };

  if (!identify_eeprom())
    return -3;

  if (!read_eeprom_word(2, data_size) || data_size < 5) {
    fprintf(stderr, "Invalid data size found: %04X\n", data_size);
    return -4;
  }
  fprintf(stderr, "Current data size: %04X bytes\n", data_size);

  if (!read_eeprom_word(data_size + 0x30, hdr_sum)) {
    return -5;
  }
  if (!read_eeprom_word(data_size + 0x32, data_sum)) {
    return -6;
  }

  // reserved space = header size (0x30) + old checksums + new checksums
  if (data_size > (max_eeprom_address - 0x38 - sizeof(audio_format_patch))) {
    fprintf(stderr, "Current data size is too large to fit audio descriptor patch\n");
    return -7;
  }

  /* TODO: we assume the sums are correct, should probably verify them and
  *  show a warning (do not abort / fail) if they're incorrect. But it takes a long time
  *  to read all of the existing EEPROM...
  */

  // first three opcodes must be "mov DPTR, #i; mov A,R7; movx @DPTR,A" (5 bytes)
  uint32_t op;
  uint8_t op2;
  if (!read_eeprom_dword(0x30, op) || !read_eeprom_byte(0x34, op2))
    return -8;
  if ((op & 0xFF0000FF) != 0x900000EF || op2 != 0xF0) {
    fprintf(stderr, "Unexpected bytestream, don't know how to patch this: %08X%02X\n", op, op2);
    if ((op & 0xFF0000FF) == 0x120000EF)
      fprintf(stderr, "This device may have already been patched.\n");
    return -9;
  }
  op = (uint16_t)(op >> 8);

  fprintf(stderr, "Found Patch_Common start, DPTR immediate is %04X\n", op);
  // for safety don't overwrite old checksums - reduces the chance of leaving the EEPROM invalid if something goes wrong
  // so the old checksums now are included in the data...
  data_sum = data_sum + (hdr_sum >> 8) + (uint8_t)hdr_sum + (data_sum >> 8) + (uint8_t)data_sum;
  // the old data size will be replaced
  hdr_sum -= data_size >> 8;
  hdr_sum -= (uint8_t)data_size;
  // add 4 bytes to cover the old sums
  data_size += 4;
  // new code will go here:
  uint16_t patch_offset = 0xCC00 + data_size;

  // update data_sum with added data
  data_sum = data_sum - 0x90 + 0x12; // first opcode "mov DPTR, #i" will be changed to "lcall XXXX"
  data_sum += patch_offset >> 8;     // remainder of opcode is the code offset for the patch
  data_sum += (uint8_t)patch_offset;
  for (int i = 0; i < sizeof(audio_format_patch); i++) {
    data_sum += audio_format_patch[i];
  }

  /* Insert original DPTR address into patch.
  *  These bytes were originally zeroed in the patch so they didn't add to the data sum,
  *  and the original sum already included them - we just relocated them from the
  *  beginning of the code to our patch, so they don't need to be summed again.
  */
  audio_format_patch[sizeof(audio_format_patch) - 3] = op >> 8;
  audio_format_patch[sizeof(audio_format_patch) - 2] = (uint8_t)op;

  // write patch while updating size
  fprintf(stderr, "Adding audio descriptor patch code\n");
  for (int i = 0; i < sizeof(audio_format_patch); i++) {
    if (!write_eeprom_byte(data_size+0x30, audio_format_patch[i]))
      return -10;
    ++data_size;
  }

  hdr_sum += data_size >> 8;
  hdr_sum += (uint8_t)data_size;
  // write out the new checksums first before updating data size
  fprintf(stderr, "Updating header checksum (%04X)\n", hdr_sum);
  if (!write_eeprom_word(data_size + 0x30, hdr_sum))
    return -11;
  fprintf(stderr, "Updating data checksum (%04X)\n", data_sum);
  if (!write_eeprom_word(data_size + 0x32, data_sum))
    return -12;

  // fix the data size in the header
  // DANGER: checksums are incorrect while initial opcode hasn't been overwritten!
  fprintf(stderr, "Updating code size %04X bytes\n", data_size);
  if (!write_eeprom_word(2, data_size))
    return -13;

  // overwrite the first opcode
  fprintf(stderr, "Overwriting initial opcode\n");
  if (!write_eeprom_byte(0x30, 0x12) || !write_eeprom_word(0x31, (uint16_t)patch_offset))
    return -14;

  fprintf(stderr, "\n\nPatching is complete!\n");
  return 0;
}


int main()
{
  fprintf(stderr, "MS2109 firmware patcher, searching for device...\n");
  find_device();
  if (ms2109 == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "Failed to find MS2109 device\n");
    return -100;
  }

  int ret;
  if (!identify_ms2109()) {
    fprintf(stderr, " could not confirm MS2109 chip ID!\n");
    ret = -200;
  }
  else if (!has_mono_descriptor()) {
    fprintf(stderr, " could not find mono USB audio format descriptor in XDATA; is device already patched?\n");
    ret = -300;
  }
  else {
    fprintf(stderr, "Attempting to patch device\n");

    /* apparently XDATA@F002 should be cleared whilst accessing EEPROM.
    *  Maybe this is a GPIO connected to the EEPROM's WP pin
    *  but for my devices it seems to make no difference - possibly
    *  the cheap makers didn't bother connecting it and just grounded the pin...
    */
    BOOLEAN restore_f002 = false;
    uint8_t f002;
    if (read_xdata_byte(0xF002, f002) && f002 != 0)
      restore_f002 = write_xdata_byte(0xF002, 0);

    ret = attempt_patch();
    if (restore_f002) write_xdata_byte(0xF002, f002);
  }

  CloseHandle(ms2109);
  if (ret == 0 && ms2109_instance) {
    std::basic_string<TCHAR> params;

    fprintf(stderr, "Attempting to uninstall current USB driver for ");
    _fputts(ms2109_instance, stderr);
    fprintf(stderr, " using pnputil.\nPlease ensure no other applications are currently using the USB device.\nYou will need to give administrator permission for this to succeed.\n");

    params = TEXT("/remove-device \"");
    params += ms2109_instance;
    params += TEXT("\" /subtree");

    // sigh
    void* foo;
    Wow64DisableWow64FsRedirection(&foo);

    TCHAR winpath[MAX_PATH + 1];
    if (GetWindowsDirectory(winpath, MAX_PATH) == 0) {
      // wtf...
      _tcscpy_s(winpath, TEXT("C:\\Windows"));
    }
    std::basic_string<TCHAR> filename(winpath);
    filename += TEXT("\\system32\\pnputil.exe");

    if ((int)ShellExecute(NULL, TEXT("runas"), filename.c_str(), params.c_str(), NULL, SW_SHOWNORMAL) > 32) {
      fprintf(stderr, "Old USB driver has been uninstalled.\n");
    }
    else fprintf(stderr, "Failed to execute pnputil; you may need to manually uninstall the USB drivers for the MS2109 device\n");
  }
  free(ms2109_instance);

  if (ret==0) fprintf(stderr, "\n\nMake sure to unplug/replug device for the patch to take effect!\n");
  return ret;
}
