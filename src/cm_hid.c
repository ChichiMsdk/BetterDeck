#ifndef CM_HID_C
#define CM_HID_C

#pragma warning(push, 0)
#include <windows.h>
#include <setupapi.h>
#include <Cfgmgr32.h>
#define INITGUID
#include <Devpkey.h>
#include <Propkey.h>
#include <hidsdi.h>
#include <hidclass.h>
#pragma warning(pop)

/*
 * NOTE:
 *      MAXIMUM_USB_STRING_LENGTH from usbspec.h is 255
 *      BLUETOOTH_DEVICE_NAME_SIZE from bluetoothapis.h is 256
 */
#define MAX_STRING_CHARS 256
/*
 * NOTE:
 *      For certain USB devices, using a buffer larger or equal to 127 wchars results
 *      in successful completion of HID API functions, but a broken string is stored
 *      in the output buffer. This behaviour persists even if HID API is bypassed and
 *      HID IOCTLs are passed to the HID driver directly. Therefore, for USB devices,
 *      the buffer MUST NOT exceed 126 WCHARs.
 */
#define MAX_STRING_CHARS_USB 126

#define HID_BUS_FLAG_BLE 0x01

#pragma warning(disable : 4820)
typedef enum e_HidBusType {
  HID_BUS_UNKNOWN     = 0x00,
  /* https://usb.org/hid */
  HID_BUS_USB         = 0x01,

  /*
   * https://www.bluetooth.com/specifications/specs/human-interface-device-profile-1-1-1/
   * https://www.bluetooth.com/specifications/specs/hid-service-1-0/
   * https://www.bluetooth.com/specifications/specs/hid-over-gatt-profile-1-0/
   */
  HID_BUS_BL          = 0x02, 
  
  /* https://docs.microsoft.com/previous-versions/windows/hardware/design/dn642101(v=vs.85) */
  HID_BUS_I2C         = 0x03, 

  /* https://www.microsoft.com/download/details.aspx?id=103325 */
  HID_BUS_SPI         = 0x04,

  HID_BUS_MAX
} e_HidBusType;

typedef struct HidDeviceInfo {
  char                 *path;
  u16                  vendor_id;
  u16                  product_id;
  char                 *serial_number;
  u16                  release_number;       // Binary-coded decimal, known as Device Version Number
  char                 *manufacturer_string;
  char                 *product_string;
  u16                  usage_page;           // Windows/Mac/hidraw only
  u16                  usage;                // Windows/Mac/hidraw only
  i32                  interface_number;     // Set to -1 in if not USB HID device.
  e_HidBusType         type;
  struct HidDeviceInfo *next;
} HidDeviceInfo;

typedef struct HidReport
{
  u8  *buf;
  u32 size;
} HidReport;

typedef struct HidDevice {
  HANDLE        h_dev;
  BOOL          blocking;

  HidReport     output;
  HidReport     input;
  HidReport     feature;

  BOOL          read_pending;
  OVERLAPPED    read_ol;
  DWORD         read_timeout_ms;
  OVERLAPPED    ol;
  OVERLAPPED    write_ol;
  DWORD         write_timeout_ms;
  HidDeviceInfo *device_info;
} HidDevice;

typedef struct HidDetectBusType {
	u32           flags;
	DEVINST       dev_node;
	e_HidBusType  type;
} HidDetectBusType;

#pragma warning(default : 4820)

static CONFIGRET
hid_interface_list_get_size(DWORD *len, GUID *iguid)
{
  CONFIGRET cr = CM_Get_Device_Interface_List_Size(
      len,
      iguid,
      NULL,
      CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
  if (cr != CR_SUCCESS && cr != CR_BUFFER_SMALL)
    report_error("CM_Get_Device_Interface_List");
  return cr;
}

static CONFIGRET
idev_get_list(GUID *iguid, void* buffer, DWORD len)
{
  CONFIGRET cr = CM_Get_Device_Interface_List(
      iguid,
      NULL,
      buffer,
      len,
      CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

  if (cr != CR_SUCCESS && cr != CR_BUFFER_SMALL)
    report_error("CM_Get_Device_Interface_List");
  return cr;
}

static HANDLE
hid_open_ro(char* path)
{
  HANDLE h_dev  = NULL;
  h_dev = CreateFile(
      path,
      0,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      NULL,
      OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED,
      NULL);
  /* if (!h_dev) report_error("CreateFile", path); */
  return h_dev;
}

static HANDLE
hid_open_rw(char* path)
{
  HANDLE h_dev  = NULL;
  h_dev = CreateFile(
      path,
      GENERIC_WRITE | GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      NULL,
      OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED,
      NULL);
  /* if (!h_dev) report_error("CreateFile", path); */
  return h_dev;
}

static void*
hid_interface_get_property(char *path, DEVPROPKEY *key, DEVPROPTYPE expected_type)
{
	ULONG       len   = 0;
	PBYTE       value = NULL;
	CONFIGRET   cr;
	DEVPROPTYPE property_type;

  wchar_t *wpath = chars_to_wchars(path);
	cr = CM_Get_Device_Interface_PropertyW(wpath, key, &property_type, NULL, &len, 0);
	if (cr != CR_BUFFER_SMALL || property_type != expected_type)
  {
    report_error("CM_Get_Device_Interface_PropertyW");
    heap_free_dz(wpath);
    return NULL;
  }

	heap_alloc_dz(len * sizeof(BYTE), value);
	cr = CM_Get_Device_Interface_PropertyW(wpath, key, &property_type, value, &len, 0);
	if (cr != CR_SUCCESS)
  {
    report_error("CM_Get_Device_Interface_PropertyW");
		heap_free_dz(value);
    heap_free_dz(wpath);
		return NULL;
	}
  heap_free_dz(wpath);
	return value;
}

static void*
hid_node_get_property(DEVINST dev_node, DEVPROPKEY* key, DEVPROPTYPE expected_type)
{
	ULONG       len   = 0;
	PBYTE       value = NULL;
	CONFIGRET   cr;
	DEVPROPTYPE property_type;

	cr = CM_Get_DevNode_PropertyW(dev_node, key, &property_type, NULL, &len, 0);
	if (cr != CR_BUFFER_SMALL || property_type != expected_type)
  {
    report_error("CM_Get_DevNode_PropertyW");
    return NULL;
  }

	heap_alloc_dz(len * sizeof(BYTE), value);
	cr = CM_Get_DevNode_PropertyW(dev_node, key, &property_type, value, &len, 0);
	if (cr != CR_SUCCESS)
  {
    report_error("CM_Get_DevNode_PropertyW");
		heap_free_dz(value);
		return NULL;
	}
	return value;
}

static HidDetectBusType
hid_get_bus_type(char *path)
{
  char              *device_id = NULL;
  char              *ids       = NULL;
	DEVINST           dev_node;
	CONFIGRET         cr;
	HidDetectBusType  bus         = {0};

	device_id = (char *) hid_interface_get_property(
      path,
      &DEVPKEY_Device_InstanceId,
      DEVPROP_TYPE_STRING);

	if (!device_id) goto end;

	cr = CM_Locate_DevNode(&dev_node, (DEVINSTID)device_id, CM_LOCATE_DEVNODE_NORMAL);
	if (cr != CR_SUCCESS) goto end;

	cr = CM_Get_Parent(&dev_node, dev_node, 0);
	if (cr != CR_SUCCESS) goto end;

	/* NOTE: Get the compatible ids from parent devnode */
  ids = (char *) hid_node_get_property(dev_node,
      &DEVPKEY_Device_CompatibleIds,
      DEVPROP_TYPE_STRING_LIST);

	if (!ids) goto end;

	/* NOTE: Now we can parse parent's compatible IDs to find out the device bus type */
	for (char *id = ids; *id; id += strlen(id) + 1)
  {
    for (char* i = id; *i; ++i) *i = (char) toupper(*i);
		/* https://docs.microsoft.com/windows-hardware/drivers/hid/plug-and-play-support
     * https://docs.microsoft.com/windows-hardware/drivers/install/standard-usb-identifiers */
		if (strstr(id, "USB")) { bus.type = HID_BUS_USB; break; }

		/* https://docs.microsoft.com/windows-hardware/drivers/bluetooth/installing-a-bluetooth-device */
		if (strstr(id, "BTHENUM")) { bus.type = HID_BUS_BL; break; }
		if (strstr(id, "BTHLEDEVICE")) { bus.type = HID_BUS_BL; bus.flags |= HID_BUS_FLAG_BLE; break; }

		/* I2C devices https://docs.microsoft.com/windows-hardware/drivers/hid/plug-and-play-support-and-power-management */
		if (strstr(id, "PNP0C50")) { bus.type = HID_BUS_I2C; break; }

    /* SPI devices https://docs.microsoft.com/windows-hardware/drivers/hid/plug-and-play-for-spi */
		if (strstr(id, "PNP0C51")) { bus.type = HID_BUS_SPI; break; }
	}
	bus.dev_node = dev_node;
end:
	heap_free_dz(device_id);
	heap_free_dz(ids);
	return bus;
}

static int
hid_token_value_get(char *string, char *token)
{
	int   token_value;
	char  *startptr;
  char  *endptr;

	startptr = strstr(string, token);
	if (!startptr) return -1;

	startptr   += strlen(token);
	token_value = strtol(startptr, &endptr, 16);
	if (endptr == startptr) return -1;
	return token_value;
}

static void
hid_usb_get_info(HidDeviceInfo *dev, DEVINST dev_node)
{
  char *dev_id = NULL;
  char *hw_ids = NULL;

	dev_id = (char *)hid_node_get_property(dev_node, &DEVPKEY_Device_InstanceId, DEVPROP_TYPE_STRING);
	if (!dev_id) goto end;

  for (char* i = dev_id; *i; ++i) *i= (char) toupper(*i);
  /*
   * NOTE:
   *      Check for Xbox Common Controller class (XUSB) device.
   *      https://docs.microsoft.com/windows/win32/xinput/directinput-and-xusb-devices
   *      https://docs.microsoft.com/windows/win32/xinput/xinput-and-directinput
   */
	if (hid_token_value_get(dev_id, "IG_") != -1)
  {
		/* NOTE: Get devnode parent to reach out USB device. */
		if (CM_Get_Parent(&dev_node, dev_node, 0) != CR_SUCCESS) goto end;
	}

	hw_ids = (char *)hid_node_get_property(dev_node, &DEVPKEY_Device_HardwareIds, DEVPROP_TYPE_STRING_LIST);
	if (!hw_ids) goto end;
  /*
   * NOTE:
   *      Get additional information from USB device's Hardware ID
   *      https://docs.microsoft.com/windows-hardware/drivers/install/standard-usb-identifiers
   *      https://docs.microsoft.com/windows-hardware/drivers/usbcon/enumeration-of-interfaces-not-grouped-in-collections
   */
  for (char *hw_id = hw_ids; *hw_id; hw_id += strlen(hw_id) + 1)
  {
    for (char *i = hw_id; *i; ++i) *i = (char) toupper(*i);
    if (dev->release_number == 0)
    {
      /* USB_DEVICE_DESCRIPTOR.bcdDevice value. */
      int release_number = hid_token_value_get(hw_id, "REV_");
      if (release_number != -1) dev->release_number = (u16) release_number;
    }

    if (dev->interface_number == -1)
    {
      /* NOTE: USB_INTERFACE_DESCRIPTOR.bInterfaceNumber value. */
      int interface_number = hid_token_value_get(hw_id, "MI_");
      if (interface_number != -1) dev->interface_number = interface_number;
    }
  }

  /* NOTE: Try to get USB device manufacturer string if not provided by HidD_GetManufacturerString. */
  if (!strlen(dev->manufacturer_string))
  {
    char *m_str = (char *) hid_node_get_property(dev_node, &DEVPKEY_Device_Manufacturer, DEVPROP_TYPE_STRING);
    if (m_str)
    {
      heap_free_dz(dev->manufacturer_string);
      dev->manufacturer_string = m_str;
    }
  }

  /* NOTE: Try to get USB device serial number if not provided by HidD_GetSerialNumberString. */
  if (!strlen(dev->serial_number))
  {
    DEVINST usb_dev_node = dev_node;
    if (dev->interface_number != -1)
    {
      /*
       * NOTE:
       *       Get devnode parent to reach out composite parent USB device.
       *       https://docs.microsoft.com/windows-hardware/drivers/usbcon/enumeration-of-the-composite-parent-device
       */
      if (CM_Get_Parent(&usb_dev_node, dev_node, 0) != CR_SUCCESS) goto end;
    }
    /* NOTE: Get the device id of the USB device. */
    heap_free_dz(dev_id);
    dev_id = (char *)hid_node_get_property(usb_dev_node, &DEVPKEY_Device_InstanceId, DEVPROP_TYPE_STRING);
    if (!dev_id) goto end;
    /*
     * NOTE:
     *       Extract substring after last '\\' of Instance ID.
     *       For USB devices it may contain device's serial number.
     *       https://docs.microsoft.com/windows-hardware/drivers/install/instance-ids
     */
    for (char *ptr = dev_id + strlen(dev_id); ptr > dev_id; --ptr)
    {
      /*
       * NOTE:
       *      Instance ID is unique only within the scope of the bus.
       *      For USB devices it means that serial number is not available. Skip.
       */
      if (*ptr == '&') break;

      if (*ptr == '\\')
      {
        heap_free_dz(dev->serial_number);
        dev->serial_number = _strdup(ptr + 1);
        break;
      }
    }
  }
  /* NOTE: If we can't get the interface number, it means that there is only one interface. */
  if (dev->interface_number == -1) dev->interface_number = 0;
end:
  heap_free_dz(dev_id);
  heap_free_dz(hw_ids);
}

static void
hid_ble_get_info(HidDeviceInfo *dev, DEVINST dev_node)
{
	if (strlen(dev->manufacturer_string) == 0)
  {
		/* NOTE: Manufacturer Name String (UUID: 0x2A29) */
		char *m_string = (char *)hid_node_get_property(
        dev_node,
        (DEVPROPKEY*)&PKEY_DeviceInterface_Bluetooth_Manufacturer,
        DEVPROP_TYPE_STRING);

		if (m_string)
    {
			heap_free_dz(dev->manufacturer_string);
			dev->manufacturer_string = m_string;
		}
	}
	if (strlen(dev->serial_number) == 0)
  {
		/* NOTE: Serial Number String (UUID: 0x2A25) */
		char *serial_number = (char *)hid_node_get_property(
        dev_node,
        (DEVPROPKEY*)&PKEY_DeviceInterface_Bluetooth_DeviceAddress,
        DEVPROP_TYPE_STRING);

		if (serial_number)
    {
			heap_free_dz(dev->serial_number);
			dev->serial_number = serial_number;
		}
	}
	if (strlen(dev->product_string) == 0)
  {
		/* NOTE: Model Number String (UUID: 0x2A24) */
		char *product_string = (char *)hid_node_get_property(
        dev_node,
        (DEVPROPKEY*)&PKEY_DeviceInterface_Bluetooth_ModelNumber,
        DEVPROP_TYPE_STRING);

		if (!product_string)
    {
			DEVINST parent_dev_node = 0;
			/* NOTE: Fallback: Get devnode grandparent to reach out Bluetooth LE device node */
			if (CM_Get_Parent(&parent_dev_node, dev_node, 0) == CR_SUCCESS)
      {
				/* NOTE: Device Name (UUID: 0x2A00) */
				product_string = (char *) hid_node_get_property(
            parent_dev_node,
            &DEVPKEY_NAME,
            DEVPROP_TYPE_STRING);
			}
		}
		if (product_string)
    {
			heap_free_dz(dev->product_string);
			dev->product_string = product_string;
		}
	}
}

static void
hid_close_info(HidDeviceInfo* info)
{
  HidDeviceInfo *d = info;
  while (d)
  {
    HidDeviceInfo *next = d->next;
    heap_free_dz(d->serial_number);
    heap_free_dz(d->product_string);
    heap_free_dz(d->manufacturer_string);
    heap_free_dz(d);
    d = next;
  }
}

static void
hid_close_device(HidDevice* dev)
{
  if (!dev)
    return ;

	if (!CancelIo(dev->h_dev)) report_error("CancelIo");

	handle_close(dev->h_dev);
	handle_close(dev->ol.hEvent);
	handle_close(dev->read_ol.hEvent);
	handle_close(dev->write_ol.hEvent);

	heap_free_dz(dev->input.buf);
	heap_free_dz(dev->output.buf);
	heap_free_dz(dev->feature.buf);

	hid_close_info(dev->device_info);
	heap_free_dz(dev);
}

static HidDevice*
hid_get_device(HidDeviceInfo *hid_info)
{
	HANDLE                h_dev   = INVALID_HANDLE_VALUE;
	HIDP_CAPS             caps    = {0};
	HidDevice             *dev    = NULL;
	PHIDP_PREPARSED_DATA  pp_data = NULL;

  /* NOTE: System devices, keyboards, mice, cannot be opened in rw */
	h_dev = hid_open_rw(hid_info->path);
	if (h_dev == INVALID_HANDLE_VALUE)
  { report_error("hid_open_ro", hid_info->path); goto cleanup; }

	/* Set the Input Report buffer size to 64 reports. */
	if (!HidD_SetNumInputBuffers(h_dev, 64))
  { console_debug("HidD_SetNumInputBuffers"); goto cleanup; }

	/* Get the Input Report length for the device. */
	if (!HidD_GetPreparsedData(h_dev, &pp_data))
  { console_debug("HidD_GetPreparsedData"); goto cleanup; }

	if (HidP_GetCaps(pp_data, &caps) != HIDP_STATUS_SUCCESS)
  { console_debug("HidP_GetCaps"); goto cleanup; }

  heap_alloc_dz(sizeof(HidDevice), dev);
	dev->h_dev = h_dev;
	h_dev      = NULL;

  /* NOTE: ensuring union is zero'ed */
  memset(&dev->ol, 0, sizeof(dev->ol));
  memset(&dev->read_ol, 0, sizeof(dev->read_ol));
  memset(&dev->write_ol, 0, sizeof(dev->write_ol));

  dev->blocking         = TRUE;
  dev->read_pending     = FALSE;
  /* FIXME: Handle errors for CreateEvent! */
  dev->ol.hEvent        = CreateEvent(NULL, FALSE, FALSE, NULL);
  dev->read_ol.hEvent   = CreateEvent(NULL, FALSE, FALSE, NULL);
  dev->write_ol.hEvent  = CreateEvent(NULL, FALSE, FALSE, NULL);
  dev->write_timeout_ms = 1000;
  dev->read_timeout_ms  = 1000;

  dev->device_info  = hid_info;
  dev->input.size   = caps.InputReportByteLength;
  dev->output.size  = caps.OutputReportByteLength;
  dev->feature.size = caps.FeatureReportByteLength;

	heap_alloc_dz(dev->input.size, dev->input.buf);
	heap_alloc_dz(dev->output.size, dev->output.buf);
	heap_alloc_dz(dev->feature.size, dev->feature.buf);

cleanup:
	if (h_dev)   handle_close(h_dev);
	if (pp_data) HidD_FreePreparsedData(pp_data);
	return dev;
}

static HidDeviceInfo*
hid_get_info(char *path, HANDLE h)
{
	HIDP_CAPS             caps;
	char                  string[MAX_STRING_CHARS + 1];
	ULONG                 len     = 0;
	ULONG                 size    = 0;
	HidDeviceInfo         *dev    = NULL;
	HIDD_ATTRIBUTES       attrib  = {.Size  = sizeof(HIDD_ATTRIBUTES),};
	HidDetectBusType      dbtype  = {0};
	PHIDP_PREPARSED_DATA  pp_data = NULL;

  heap_alloc_dz(sizeof(HidDeviceInfo), dev);
	dev->next             = NULL;
	dev->path             = path;
	dev->interface_number = -1;

	if (HidD_GetAttributes(h, &attrib))
  {
		dev->vendor_id      = attrib.VendorID;
		dev->product_id     = attrib.ProductID;
		dev->release_number = attrib.VersionNumber;
	}
  else console_debug("HidD_GetAttributes");

	if (HidD_GetPreparsedData(h, &pp_data))
  {
		if (HidP_GetCaps(pp_data, &caps) == HIDP_STATUS_SUCCESS)
    {
			dev->usage      = caps.Usage;
			dev->usage_page = caps.UsagePage;
		}
    else console_debug("HidD_GetCaps");
		HidD_FreePreparsedData(pp_data);
	}
  else console_debug("HidD_GetPreparsedData");

	/* NOTE: detect bus type before reading string descriptors */
	dbtype      = hid_get_bus_type(path);
	dev->type   = dbtype.type;

	len         = (dev->type == HID_BUS_USB) ? MAX_STRING_CHARS_USB : MAX_STRING_CHARS;
	size        = len * sizeof(char);
	string[len] = '\0';

	string[0] = '\0';
	if (!HidD_GetSerialNumberString(h, string, size)) console_debug("HidD_GetSerialNumberString");
	dev->serial_number = _strdup(string);

	string[0] = '\0';
	if (!HidD_GetManufacturerString(h, string, size)) console_debug("HidD_GetManufacturerString");
	dev->manufacturer_string = _strdup(string);

	string[0] = '\0';
	if (!HidD_GetProductString(h, string, size)) console_debug("HidD_GetProductString");
	dev->product_string = _strdup(string);

  /* NOTE: String descriptors */
  switch (dev->type)
  {
    case HID_BUS_USB: 
    {
      hid_usb_get_info(dev, dbtype.dev_node);
      break;
    }
    case HID_BUS_BL:
    {
      if (dbtype.flags & HID_BUS_FLAG_BLE)
        hid_ble_get_info(dev, dbtype.dev_node);
      break;
    }
    case HID_BUS_UNKNOWN:
    case HID_BUS_SPI:
    case HID_BUS_I2C:
    case HID_BUS_MAX:
    default: break;
	}
	return dev;
}

static HidDeviceInfo*
hid_enumerate(u16 v_id, u16 p_id)
{
  char          *idev_list   = NULL;
  GUID          iguid        = {0};
  DWORD         len          = 0;
  DWORD         required_len = 0;
  CONFIGRET     cr           = 0;
  HidDeviceInfo *root        = NULL;
  HidDeviceInfo *current     = NULL;

  HidD_GetHidGuid(&iguid);
  do {
    cr = hid_interface_list_get_size(&len, &iguid);
    if (cr != CR_SUCCESS) break;

    if (idev_list) heap_free_dz(idev_list);
    heap_alloc_dz(len * sizeof(char) + 1, idev_list);

    cr = idev_get_list(&iguid, idev_list, len);

  } while(cr == CR_BUFFER_SMALL);

  if (cr != CR_SUCCESS) goto cleanup;

  for (char *idev = idev_list; *idev; idev += strlen(idev) + 1)
  {
		HANDLE          h_dev   = INVALID_HANDLE_VALUE;
		HIDD_ATTRIBUTES attrib  = {.Size = sizeof(HIDD_ATTRIBUTES)};

		h_dev = hid_open_rw(idev);
		if (h_dev == INVALID_HANDLE_VALUE) continue;

    if (HidD_GetAttributes(h_dev, &attrib))
    {
      if ( (v_id == 0x0 || attrib.VendorID == v_id)
          && (p_id == 0x0 || attrib.ProductID == p_id) )
      {
        HidDeviceInfo *tmp = hid_get_info(idev, h_dev);
        if (tmp)
        {
          if (current) current->next = tmp;
          else root = tmp;
          current = tmp;
        }
      }
		}
    else console_debug("HidD_GetAttributes");
    /* FIXME: What if this fails ? */
    handle_close(h_dev);
  }
cleanup:
  heap_free_dz(idev_list);
  return root;
}

#define HID_SEND_FEATURE  0x10
#define HID_SEND_OUTPUT   0x11

static i64
hid_send_report(HidDevice* hid_dev, HidReport data, int type)
{
  HidReport payload     = data;
  HidReport dev_payload = {0};

	if      (!data.buf || !data.size)  return -1;
  switch(type)
  {
    case HID_SEND_FEATURE : dev_payload = hid_dev->feature; break;
    case HID_SEND_OUTPUT  : dev_payload = hid_dev->output;  break;
    default: printf("Wrong payload type specified (%d)\n", type); return -1;
  }
  /*
   * NOTE:
   *      At least caps.FeatureReportByteLength to HidD_SetFeature(), even if shorter. 
   *      If less, ERROR_INVALID_PARAMETER. 
   *      If more silently truncates the data sent to caps.FeatureReportByteLength
   */
	if (data.size <= dev_payload.size)
  {
    payload = dev_payload;
		memcpy(payload.buf, data.buf, data.size);
		memset(payload.buf + data.size, 0, payload.size - data.size);
	}
	if (!HidD_SetFeature(hid_dev->h_dev, (PVOID)payload.buf, (DWORD) payload.size))
  {
    report_error("HidD_SetFeature");
    return -1;
  }
	return payload.size;
}

static i64
hid_get_report(HANDLE h, HidReport d, int type)
{
	if (!d.buf || !d.size) return -1;

  BOOL        wait  = TRUE;
	DWORD       total = 0;
	OVERLAPPED  ol    = {0};
  /* NOTE: Ensure union is also zero'ed */
	memset(&ol, 0, sizeof(ol));

  d.buf[0] = 0x06;
  if (!DeviceIoControl(h, type, d.buf, d.size, d.buf, d.size, &total, &ol))
  {
		if (GetLastError() != ERROR_IO_PENDING)
    {
      report_error("DeviceIoControl");
      return -1;
    }
  }
	if (!GetOverlappedResult(h, &ol, &total, wait))
  {
    report_error("GetOverLappedResult");
		return -1;
	}
  /* NOTE: If no numbered reports, `total` does not seem to include the first byte with 0 */
	if (d.buf[0] == 0x0) total++;
	return total;
}

#define hid_get_feature_report(h, d) hid_get_report((h), (d), IOCTL_HID_GET_FEATURE)
#define hid_get_input_report(h, d) hid_get_report((h), (d), IOCTL_HID_GET_INPUT_REPORT)

static i64
hid_read(HidDevice* hid_dev, HidReport data)
{
  DWORD read  = 0;
  if (!ReadFile(hid_dev->h_dev, data.buf, (DWORD) data.size, &read, &hid_dev->read_ol))
  {
    if (GetLastError() != ERROR_IO_PENDING)
    {
      report_error("ReadFile");
      return -1;
    }
    if (WaitForSingleObject(hid_dev->read_ol.hEvent, hid_dev->read_timeout_ms) != WAIT_OBJECT_0)
    {
      report_error("WaitForSingleObject");
      return -1;
    }
    if (!GetOverlappedResult(hid_dev->h_dev, &hid_dev->read_ol, &read, FALSE))
    {
      report_error("GetOverlappedResult");
      return -1;
    }
  }
  return read;
}

static i64
hid_write(HidDevice* hid_dev, HidReport data)
{
  DWORD     written    = 0;
  HidReport payload    = data;
	if (!data.size || !data.buf) return -1;
  /*
   * NOTE:
   *      At least caps.OutputReportByteLength + 1 to WriteFile(), even if shorter. 
   *      If less, ERROR_INVALID_PARAMETER. 
   *      If more silently truncates the data sent to caps.FeatureReportByteLength
   */
  if (payload.size <= hid_dev->output.size)
  {
    payload = hid_dev->output;
    memcpy(payload.buf, data.buf, data.size);
    memset(payload.buf + data.size, 0, payload.size - data.size);
  }
	if (!WriteFile(hid_dev->h_dev, payload.buf, (DWORD)payload.size, &written, &hid_dev->write_ol))
  {
    if (GetLastError() != ERROR_IO_PENDING)
    {
      report_error("WriteFile");
      return -1;
    }
		if (WaitForSingleObject(hid_dev->write_ol.hEvent, hid_dev->write_timeout_ms) != WAIT_OBJECT_0)
    {
			report_error("WaitForSingleObject");
			return -1;
		}
		if (!GetOverlappedResult(hid_dev->h_dev, &hid_dev->write_ol, &written, FALSE))
    {
			report_error("GetOverlappedResult");
			return -1;
		}
	}
	return written;
}

#endif // CM_HID_C
