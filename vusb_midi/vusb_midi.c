/*
 * vusb_midi.c - Virtual USB MIDI Gadget for Kronos
 *
 * Creates a USB MIDI Class 1.0 device on the dummy_hcd virtual bus.
 * The Kronos USBMidiAccessory module discovers it as a standard USB MIDI
 * controller — no reverse engineering of Korg modules required.
 *
 * MIDI injection: write raw MIDI bytes to /proc/.vmidi
 *   e.g.  echo -ne '\xf0\x7e\x7f\x06\x01\xf7' > /proc/.vmidi
 *
 * Architecture:
 *   Network → daemon → /proc/.vmidi → ring buffer → USB MIDI packets
 *   → dummy_hcd virtual bus → USBMidiAccessory → Kronos EVA
 *
 * Based on gmidi.c by Ben Williamson (Thumtronics/Grey Innovation).
 * ALSA dependency removed; replaced with /proc injection interface.
 *
 * RTAI note: GFP_KERNEL and create_proc_entry fail in init_module on
 * this kernel.  All setup is deferred via schedule_delayed_work().
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/utsname.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <asm/uaccess.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/audio.h>
#include <linux/usb/midi.h>

#include "gadget_chips.h"

#include <linux/vmalloc.h>
#define kmalloc(size, flags) vmalloc(size)
#define kfree(ptr) vfree(ptr)

#include "usbstring.c"
#include "config.c"
#include "epautoconf.c"

#undef kmalloc
#undef kfree

/*-------------------------------------------------------------------------*/
/* Configuration                                                           */
/*-------------------------------------------------------------------------*/

#define VMIDI_VENDOR_ID     0x1209  /* pid.codes open-source VID */
#define VMIDI_PRODUCT_ID    0x0001
#define VMIDI_CONFIG        1
#define VMIDI_AC_IFACE      0
#define VMIDI_MS_IFACE      1
#define VMIDI_NUM_IFACES    2

#define VMIDI_EP_BUFSIZE    256
#define VMIDI_EP_QLEN       32
#define MIDI_RING_SIZE      4096
#define MIDI_RING_MASK      (MIDI_RING_SIZE - 1)

#define USB_BUFSIZ          256

#define STRING_MANUFACTURER 25
#define STRING_PRODUCT      42
#define STRING_SERIAL       101
#define STRING_MIDI_GADGET  250

static const char shortname[] = "g_vmidi";
static const char longname[]  = "Kronos Virtual MIDI";

static const char *EP_IN_NAME;
static const char *EP_OUT_NAME;

/*-------------------------------------------------------------------------*/
/* MIDI state machine (USB MIDI 1.0 packet encoding)                       */
/*-------------------------------------------------------------------------*/

#define STATE_UNKNOWN   0
#define STATE_1PARAM    1
#define STATE_2PARAM_1  2
#define STATE_2PARAM_2  3
#define STATE_SYSEX_0   4
#define STATE_SYSEX_1   5
#define STATE_SYSEX_2   6

struct vmidi_in_port {
	uint8_t cable;
	uint8_t state;
	uint8_t data[2];
};

struct vmidi_device {
	spinlock_t		lock;
	struct usb_gadget	*gadget;
	struct usb_request	*req;
	u8			config;
	struct usb_ep		*in_ep, *out_ep;

	struct vmidi_in_port	in_port;
	struct tasklet_struct	tasklet;

	/* ring buffer fed by /proc/.vmidi, drained by USB IN tasklet */
	uint8_t			midi_buf[MIDI_RING_SIZE];
	unsigned int		midi_head;
	unsigned int		midi_tail;
};

static struct vmidi_device	*g_vmidi;
static struct proc_dir_entry	*vmidi_proc;

/* Static storage for vmidi_device (Kronos kernel lacks slab exports) */
static struct vmidi_device vmidi_dev_storage;

/* Static buffer pool for endpoint request buffers */
#define EP_BUF_POOL_SIZE 40
#define EP_BUF_SIZE 256
static char ep_buf_pool[EP_BUF_POOL_SIZE][EP_BUF_SIZE];
static unsigned long ep_buf_used[BITS_TO_LONGS(EP_BUF_POOL_SIZE)];
static spinlock_t ep_buf_lock = SPIN_LOCK_UNLOCKED;

static void vmidi_transmit(struct vmidi_device *dev, struct usb_request *req);
static void vmidi_complete(struct usb_ep *ep, struct usb_request *req);

/*-------------------------------------------------------------------------*/
/* USB Descriptors (USB MIDI Class 1.0)                                    */
/*-------------------------------------------------------------------------*/

DECLARE_UAC_AC_HEADER_DESCRIPTOR(1);
DECLARE_USB_MIDI_OUT_JACK_DESCRIPTOR(1);
DECLARE_USB_MS_ENDPOINT_DESCRIPTOR(1);

static struct usb_device_descriptor device_desc = {
	.bLength            = USB_DT_DEVICE_SIZE,
	.bDescriptorType    = USB_DT_DEVICE,
	.bcdUSB             = cpu_to_le16(0x0200),
	.bDeviceClass       = USB_CLASS_PER_INTERFACE,
	.idVendor           = cpu_to_le16(VMIDI_VENDOR_ID),
	.idProduct          = cpu_to_le16(VMIDI_PRODUCT_ID),
	.iManufacturer      = STRING_MANUFACTURER,
	.iProduct           = STRING_PRODUCT,
	.bNumConfigurations = 1,
};

static struct usb_config_descriptor config_desc = {
	.bLength             = USB_DT_CONFIG_SIZE,
	.bDescriptorType     = USB_DT_CONFIG,
	.bNumInterfaces      = VMIDI_NUM_IFACES,
	.bConfigurationValue = VMIDI_CONFIG,
	.iConfiguration      = STRING_MIDI_GADGET,
	.bmAttributes        = USB_CONFIG_ATT_ONE,
	.bMaxPower           = CONFIG_USB_GADGET_VBUS_DRAW / 2,
};

/* AudioControl interface (required by USB Audio Class) */
static const struct usb_interface_descriptor ac_interface_desc = {
	.bLength            = USB_DT_INTERFACE_SIZE,
	.bDescriptorType    = USB_DT_INTERFACE,
	.bInterfaceNumber   = VMIDI_AC_IFACE,
	.bNumEndpoints      = 0,
	.bInterfaceClass    = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOCONTROL,
	.iInterface         = STRING_MIDI_GADGET,
};

static const struct uac_ac_header_descriptor_1 ac_header_desc = {
	.bLength            = UAC_DT_AC_HEADER_SIZE(1),
	.bDescriptorType    = USB_DT_CS_INTERFACE,
	.bDescriptorSubtype = USB_MS_HEADER,
	.bcdADC             = cpu_to_le16(0x0100),
	.wTotalLength       = cpu_to_le16(UAC_DT_AC_HEADER_SIZE(1)),
	.bInCollection      = 1,
	.baInterfaceNr      = { [0] = VMIDI_MS_IFACE },
};

/* MIDIStreaming interface */
static const struct usb_interface_descriptor ms_interface_desc = {
	.bLength            = USB_DT_INTERFACE_SIZE,
	.bDescriptorType    = USB_DT_INTERFACE,
	.bInterfaceNumber   = VMIDI_MS_IFACE,
	.bNumEndpoints      = 2,
	.bInterfaceClass    = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_MIDISTREAMING,
	.iInterface         = STRING_MIDI_GADGET,
};

static const struct usb_ms_header_descriptor ms_header_desc = {
	.bLength            = USB_DT_MS_HEADER_SIZE,
	.bDescriptorType    = USB_DT_CS_INTERFACE,
	.bDescriptorSubtype = USB_MS_HEADER,
	.bcdMSC             = cpu_to_le16(0x0100),
	.wTotalLength       = cpu_to_le16(USB_DT_MS_HEADER_SIZE
				+ 2 * USB_DT_MIDI_IN_SIZE
				+ 2 * USB_DT_MIDI_OUT_SIZE(1)),
};

#define JACK_IN_EMB     1
#define JACK_IN_EXT     2
#define JACK_OUT_EMB    3
#define JACK_OUT_EXT    4

static const struct usb_midi_in_jack_descriptor jack_in_emb_desc = {
	.bLength            = USB_DT_MIDI_IN_SIZE,
	.bDescriptorType    = USB_DT_CS_INTERFACE,
	.bDescriptorSubtype = USB_MS_MIDI_IN_JACK,
	.bJackType          = USB_MS_EMBEDDED,
	.bJackID            = JACK_IN_EMB,
};

static const struct usb_midi_in_jack_descriptor jack_in_ext_desc = {
	.bLength            = USB_DT_MIDI_IN_SIZE,
	.bDescriptorType    = USB_DT_CS_INTERFACE,
	.bDescriptorSubtype = USB_MS_MIDI_IN_JACK,
	.bJackType          = USB_MS_EXTERNAL,
	.bJackID            = JACK_IN_EXT,
};

static const struct usb_midi_out_jack_descriptor_1 jack_out_emb_desc = {
	.bLength            = USB_DT_MIDI_OUT_SIZE(1),
	.bDescriptorType    = USB_DT_CS_INTERFACE,
	.bDescriptorSubtype = USB_MS_MIDI_OUT_JACK,
	.bJackType          = USB_MS_EMBEDDED,
	.bJackID            = JACK_OUT_EMB,
	.bNrInputPins       = 1,
	.pins               = { [0] = { .baSourceID = JACK_IN_EXT,
					 .baSourcePin = 1 } },
};

static const struct usb_midi_out_jack_descriptor_1 jack_out_ext_desc = {
	.bLength            = USB_DT_MIDI_OUT_SIZE(1),
	.bDescriptorType    = USB_DT_CS_INTERFACE,
	.bDescriptorSubtype = USB_MS_MIDI_OUT_JACK,
	.bJackType          = USB_MS_EXTERNAL,
	.bJackID            = JACK_OUT_EXT,
	.bNrInputPins       = 1,
	.pins               = { [0] = { .baSourceID = JACK_IN_EMB,
					 .baSourcePin = 1 } },
};

/* Bulk OUT endpoint (host → device: MIDI from Kronos, if needed) */
static struct usb_endpoint_descriptor bulk_out_desc = {
	.bLength          = USB_DT_ENDPOINT_AUDIO_SIZE,
	.bDescriptorType  = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes     = USB_ENDPOINT_XFER_BULK,
};

static const struct usb_ms_endpoint_descriptor_1 ms_out_desc = {
	.bLength            = USB_DT_MS_ENDPOINT_SIZE(1),
	.bDescriptorType    = USB_DT_CS_ENDPOINT,
	.bDescriptorSubtype = USB_MS_GENERAL,
	.bNumEmbMIDIJack    = 1,
	.baAssocJackID      = { [0] = JACK_IN_EMB },
};

/* Bulk IN endpoint (device → host: our injected MIDI → Kronos) */
static struct usb_endpoint_descriptor bulk_in_desc = {
	.bLength          = USB_DT_ENDPOINT_AUDIO_SIZE,
	.bDescriptorType  = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes     = USB_ENDPOINT_XFER_BULK,
};

static const struct usb_ms_endpoint_descriptor_1 ms_in_desc = {
	.bLength            = USB_DT_MS_ENDPOINT_SIZE(1),
	.bDescriptorType    = USB_DT_CS_ENDPOINT,
	.bDescriptorSubtype = USB_MS_GENERAL,
	.bNumEmbMIDIJack    = 1,
	.baAssocJackID      = { [0] = JACK_OUT_EMB },
};

static const struct usb_descriptor_header *vmidi_function[] = {
	(struct usb_descriptor_header *)&ac_interface_desc,
	(struct usb_descriptor_header *)&ac_header_desc,
	(struct usb_descriptor_header *)&ms_interface_desc,
	(struct usb_descriptor_header *)&ms_header_desc,
	(struct usb_descriptor_header *)&jack_in_emb_desc,
	(struct usb_descriptor_header *)&jack_in_ext_desc,
	(struct usb_descriptor_header *)&jack_out_emb_desc,
	(struct usb_descriptor_header *)&jack_out_ext_desc,
	(struct usb_descriptor_header *)&bulk_out_desc,
	(struct usb_descriptor_header *)&ms_out_desc,
	(struct usb_descriptor_header *)&bulk_in_desc,
	(struct usb_descriptor_header *)&ms_in_desc,
	NULL,
};

static char manufacturer[50];
static char product_desc[40] = "Kronos Virtual MIDI";
static char serial_number[20] = "0001";

static struct usb_string strings[] = {
	{ STRING_MANUFACTURER, manufacturer },
	{ STRING_PRODUCT,      product_desc },
	{ STRING_SERIAL,       serial_number },
	{ STRING_MIDI_GADGET,  (char *)longname },
	{ }
};

static struct usb_gadget_strings stringtab = {
	.language = 0x0409,
	.strings  = strings,
};

/*-------------------------------------------------------------------------*/
/* Helpers                                                                 */
/*-------------------------------------------------------------------------*/

static int config_buf(struct usb_gadget *gadget, u8 *buf, u8 type, unsigned index)
{
	int len;

	if (index != 0)
		return -EINVAL;
	len = usb_gadget_config_buf(&config_desc, buf, USB_BUFSIZ,
				    vmidi_function);
	if (len < 0)
		return len;
	((struct usb_config_descriptor *)buf)->bDescriptorType = type;
	return len;
}

static struct usb_request *alloc_ep_req(struct usb_ep *ep, unsigned length)
{
	struct usb_request *req;

	req = usb_ep_alloc_request(ep, GFP_ATOMIC);
	if (req) {
		int idx;
		unsigned long flags;
		req->length = length;
		if (length > EP_BUF_SIZE) {
			usb_ep_free_request(ep, req);
			return NULL;
		}
		spin_lock_irqsave(&ep_buf_lock, flags);
		idx = find_first_zero_bit(ep_buf_used, EP_BUF_POOL_SIZE);
		if (idx < EP_BUF_POOL_SIZE)
			set_bit(idx, ep_buf_used);
		spin_unlock_irqrestore(&ep_buf_lock, flags);
		if (idx >= EP_BUF_POOL_SIZE) {
			usb_ep_free_request(ep, req);
			return NULL;
		}
		req->buf = ep_buf_pool[idx];
		memset(req->buf, 0, length);
	}
	return req;
}

static void free_ep_req(struct usb_ep *ep, struct usb_request *req)
{
	{
		int idx = ((char *)req->buf - (char *)ep_buf_pool) / EP_BUF_SIZE;
		unsigned long flags;
		spin_lock_irqsave(&ep_buf_lock, flags);
		if (idx >= 0 && idx < EP_BUF_POOL_SIZE)
			clear_bit(idx, ep_buf_used);
		spin_unlock_irqrestore(&ep_buf_lock, flags);
	}
	usb_ep_free_request(ep, req);
}

/*-------------------------------------------------------------------------*/
/* Ring buffer                                                             */
/*-------------------------------------------------------------------------*/

static inline unsigned int ring_count(struct vmidi_device *dev)
{
	return (dev->midi_head - dev->midi_tail) & MIDI_RING_MASK;
}

static inline int ring_empty(struct vmidi_device *dev)
{
	return dev->midi_head == dev->midi_tail;
}

static inline int ring_get(struct vmidi_device *dev, uint8_t *b)
{
	if (ring_empty(dev))
		return 0;
	*b = dev->midi_buf[dev->midi_tail];
	dev->midi_tail = (dev->midi_tail + 1) & MIDI_RING_MASK;
	return 1;
}

/*-------------------------------------------------------------------------*/
/* MIDI → USB MIDI event packet conversion                                 */
/*-------------------------------------------------------------------------*/

static void vmidi_transmit_packet(struct usb_request *req,
				  uint8_t p0, uint8_t p1,
				  uint8_t p2, uint8_t p3)
{
	unsigned length = req->length;
	u8 *buf = (u8 *)req->buf + length;

	buf[0] = p0;
	buf[1] = p1;
	buf[2] = p2;
	buf[3] = p3;
	req->length = length + 4;
}

static void vmidi_transmit_byte(struct usb_request *req,
				struct vmidi_in_port *port, uint8_t b)
{
	uint8_t p0 = port->cable;

	if (b >= 0xf8) {
		vmidi_transmit_packet(req, p0 | 0x0f, b, 0, 0);
	} else if (b >= 0xf0) {
		switch (b) {
		case 0xf0:
			port->data[0] = b;
			port->state = STATE_SYSEX_1;
			break;
		case 0xf1:
		case 0xf3:
			port->data[0] = b;
			port->state = STATE_1PARAM;
			break;
		case 0xf2:
			port->data[0] = b;
			port->state = STATE_2PARAM_1;
			break;
		case 0xf4:
		case 0xf5:
			port->state = STATE_UNKNOWN;
			break;
		case 0xf6:
			vmidi_transmit_packet(req, p0 | 0x05, 0xf6, 0, 0);
			port->state = STATE_UNKNOWN;
			break;
		case 0xf7:
			switch (port->state) {
			case STATE_SYSEX_0:
				vmidi_transmit_packet(req,
					p0 | 0x05, 0xf7, 0, 0);
				break;
			case STATE_SYSEX_1:
				vmidi_transmit_packet(req,
					p0 | 0x06, port->data[0], 0xf7, 0);
				break;
			case STATE_SYSEX_2:
				vmidi_transmit_packet(req,
					p0 | 0x07, port->data[0],
					port->data[1], 0xf7);
				break;
			}
			port->state = STATE_UNKNOWN;
			break;
		}
	} else if (b >= 0x80) {
		port->data[0] = b;
		if (b >= 0xc0 && b <= 0xdf)
			port->state = STATE_1PARAM;
		else
			port->state = STATE_2PARAM_1;
	} else {
		switch (port->state) {
		case STATE_1PARAM:
			if (port->data[0] < 0xf0)
				p0 |= port->data[0] >> 4;
			else {
				p0 |= 0x02;
				port->state = STATE_UNKNOWN;
			}
			vmidi_transmit_packet(req, p0, port->data[0], b, 0);
			break;
		case STATE_2PARAM_1:
			port->data[1] = b;
			port->state = STATE_2PARAM_2;
			break;
		case STATE_2PARAM_2:
			if (port->data[0] < 0xf0) {
				p0 |= port->data[0] >> 4;
				port->state = STATE_2PARAM_1;
			} else {
				p0 |= 0x03;
				port->state = STATE_UNKNOWN;
			}
			vmidi_transmit_packet(req,
				p0, port->data[0], port->data[1], b);
			break;
		case STATE_SYSEX_0:
			port->data[0] = b;
			port->state = STATE_SYSEX_1;
			break;
		case STATE_SYSEX_1:
			port->data[1] = b;
			port->state = STATE_SYSEX_2;
			break;
		case STATE_SYSEX_2:
			vmidi_transmit_packet(req,
				p0 | 0x04, port->data[0], port->data[1], b);
			port->state = STATE_SYSEX_0;
			break;
		}
	}
}

/*-------------------------------------------------------------------------*/
/* USB MIDI transmit (drain ring buffer → IN endpoint)                     */
/*-------------------------------------------------------------------------*/

static void vmidi_transmit(struct vmidi_device *dev, struct usb_request *req)
{
	struct usb_ep *ep = dev->in_ep;
	struct vmidi_in_port *port = &dev->in_port;
	unsigned long flags;

	if (!ep)
		return;
	if (!req)
		req = alloc_ep_req(ep, VMIDI_EP_BUFSIZE);
	if (!req) {
		printk(KERN_ERR "vusb_midi: alloc_ep_req failed\n");
		return;
	}

	req->length = 0;
	req->complete = vmidi_complete;

	spin_lock_irqsave(&dev->lock, flags);
	while (req->length + 3 < VMIDI_EP_BUFSIZE) {
		uint8_t b;
		if (!ring_get(dev, &b))
			break;
		vmidi_transmit_byte(req, port, b);
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	if (req->length > 0)
		usb_ep_queue(ep, req, GFP_ATOMIC);
	else
		free_ep_req(ep, req);
}

static void vmidi_in_tasklet(unsigned long data)
{
	struct vmidi_device *dev = (struct vmidi_device *)data;
	vmidi_transmit(dev, NULL);
}

/*-------------------------------------------------------------------------*/
/* Endpoint completion                                                     */
/*-------------------------------------------------------------------------*/

static void vmidi_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct vmidi_device *dev = ep->driver_data;
	int status = req->status;

	switch (status) {
	case 0:
		if (ep == dev->out_ep) {
			/* discard data from host for now */
		} else if (ep == dev->in_ep) {
			vmidi_transmit(dev, req);
			return;
		}
		break;
	case -ECONNABORTED:
	case -ECONNRESET:
	case -ESHUTDOWN:
		free_ep_req(ep, req);
		return;
	default:
		break;
	}

	status = usb_ep_queue(ep, req, GFP_ATOMIC);
	if (status) {
		usb_ep_set_halt(ep);
	}
}

/*-------------------------------------------------------------------------*/
/* Gadget configuration                                                    */
/*-------------------------------------------------------------------------*/

static int set_vmidi_config(struct vmidi_device *dev, gfp_t gfp_flags)
{
	int err = 0;
	struct usb_request *req;
	struct usb_ep *ep;
	unsigned i;

	err = usb_ep_enable(dev->in_ep, &bulk_in_desc);
	if (err)
		goto fail;
	dev->in_ep->driver_data = dev;

	err = usb_ep_enable(dev->out_ep, &bulk_out_desc);
	if (err)
		goto fail;
	dev->out_ep->driver_data = dev;

	ep = dev->out_ep;
	for (i = 0; i < VMIDI_EP_QLEN && err == 0; i++) {
		req = alloc_ep_req(ep, VMIDI_EP_BUFSIZE);
		if (req) {
			req->complete = vmidi_complete;
			err = usb_ep_queue(ep, req, GFP_ATOMIC);
		} else {
			err = -ENOMEM;
		}
	}
fail:
	return err;
}

static void vmidi_reset_config(struct vmidi_device *dev)
{
	if (dev->config == 0)
		return;
	usb_ep_disable(dev->in_ep);
	usb_ep_disable(dev->out_ep);
	dev->config = 0;
}

static int vmidi_set_config(struct vmidi_device *dev, unsigned number,
			    gfp_t gfp_flags)
{
	int result = 0;

	vmidi_reset_config(dev);

	switch (number) {
	case VMIDI_CONFIG:
		result = set_vmidi_config(dev, gfp_flags);
		break;
	default:
		result = -EINVAL;
		/* FALL THROUGH */
	case 0:
		return result;
	}

	if (!result && (!dev->in_ep || !dev->out_ep))
		result = -ENODEV;
	if (result)
		vmidi_reset_config(dev);
	else
		dev->config = number;

	return result;
}

/*-------------------------------------------------------------------------*/
/* EP0 setup handling                                                      */
/*-------------------------------------------------------------------------*/

static void vmidi_setup_complete(struct usb_ep *ep, struct usb_request *req)
{
	/* nothing to do */
}

static int vmidi_setup(struct usb_gadget *gadget,
		       const struct usb_ctrlrequest *ctrl)
{
	printk(KERN_INFO "vusb_midi: setup bReq=%d bRT=%d wVal=0x%x wLen=%d\n",
	       ctrl->bRequest, ctrl->bRequestType,
	       le16_to_cpu(ctrl->wValue), le16_to_cpu(ctrl->wLength));
	struct vmidi_device *dev = get_gadget_data(gadget);
	struct usb_request *req = dev->req;
	int value = -EOPNOTSUPP;
	u16 w_index = le16_to_cpu(ctrl->wIndex);
	u16 w_value = le16_to_cpu(ctrl->wValue);
	u16 w_length = le16_to_cpu(ctrl->wLength);

	req->zero = 0;
	switch (ctrl->bRequest) {
	case USB_REQ_GET_DESCRIPTOR:
		if (ctrl->bRequestType != USB_DIR_IN)
			goto unknown;
		switch (w_value >> 8) {
		case USB_DT_DEVICE:
			value = min(w_length, (u16)sizeof(device_desc));
			memcpy(req->buf, &device_desc, value);
			break;
		case USB_DT_CONFIG:
			value = config_buf(gadget, req->buf,
					   w_value >> 8, w_value & 0xff);
			if (value >= 0)
				value = min(w_length, (u16)value);
			break;
		case USB_DT_STRING:
			value = usb_gadget_get_string(&stringtab,
						      w_value & 0xff,
						      req->buf);
			if (value >= 0)
				value = min(w_length, (u16)value);
			break;
		}
		break;

	case USB_REQ_SET_CONFIGURATION:
		if (ctrl->bRequestType != 0)
			goto unknown;
		spin_lock(&dev->lock);
		value = vmidi_set_config(dev, w_value, GFP_ATOMIC);
		spin_unlock(&dev->lock);
		break;

	case USB_REQ_GET_CONFIGURATION:
		if (ctrl->bRequestType != USB_DIR_IN)
			goto unknown;
		*(u8 *)req->buf = dev->config;
		value = min(w_length, (u16)1);
		break;

	case USB_REQ_SET_INTERFACE:
		if (ctrl->bRequestType != USB_RECIP_INTERFACE)
			goto unknown;
		spin_lock(&dev->lock);
		if (dev->config && w_index < VMIDI_NUM_IFACES && w_value == 0) {
			u8 config = dev->config;
			vmidi_reset_config(dev);
			vmidi_set_config(dev, config, GFP_ATOMIC);
			value = 0;
		}
		spin_unlock(&dev->lock);
		break;

	case USB_REQ_GET_INTERFACE:
		if (ctrl->bRequestType != (USB_DIR_IN | USB_RECIP_INTERFACE))
			goto unknown;
		if (!dev->config)
			break;
		if (w_index >= VMIDI_NUM_IFACES) {
			value = -EDOM;
			break;
		}
		*(u8 *)req->buf = 0;
		value = min(w_length, (u16)1);
		break;

	default:
unknown:
		break;
	}

	if (value >= 0) {
		req->length = value;
		req->zero = value < w_length;
		value = usb_ep_queue(gadget->ep0, req, GFP_ATOMIC);
		if (value < 0) {
			req->status = 0;
			vmidi_setup_complete(gadget->ep0, req);
		}
	}

	return value;
}

static void vmidi_disconnect(struct usb_gadget *gadget)
{
	struct vmidi_device *dev = get_gadget_data(gadget);
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	vmidi_reset_config(dev);
	spin_unlock_irqrestore(&dev->lock, flags);
}

static void vmidi_suspend(struct usb_gadget *gadget)
{
}

static void vmidi_resume(struct usb_gadget *gadget)
{
}

/*-------------------------------------------------------------------------*/
/* Gadget bind / unbind                                                    */
/*-------------------------------------------------------------------------*/

static void vmidi_unbind(struct usb_gadget *gadget)
{
	struct vmidi_device *dev = get_gadget_data(gadget);

	if (dev->req) {
		dev->req->length = USB_BUFSIZ;
		free_ep_req(gadget->ep0, dev->req);
	}
	g_vmidi = NULL;
	set_gadget_data(gadget, NULL);
}

static int vmidi_bind(struct usb_gadget *gadget)
{
	struct vmidi_device *dev;
	struct usb_ep *in_ep, *out_ep;
	int gcnum;

	usb_ep_autoconfig_reset(gadget);
	in_ep = usb_ep_autoconfig(gadget, &bulk_in_desc);
	if (!in_ep) {
		pr_err("vusb_midi: can't autoconfigure on %s\n", gadget->name);
		return -ENODEV;
	}
	EP_IN_NAME = in_ep->name;
	in_ep->driver_data = in_ep;

	out_ep = usb_ep_autoconfig(gadget, &bulk_out_desc);
	if (!out_ep) {
		pr_err("vusb_midi: can't autoconfigure on %s\n", gadget->name);
		return -ENODEV;
	}
	EP_OUT_NAME = out_ep->name;
	out_ep->driver_data = out_ep;

	gcnum = usb_gadget_controller_number(gadget);
	if (gcnum >= 0)
		device_desc.bcdDevice = cpu_to_le16(0x0200 + gcnum);
	else
		device_desc.bcdDevice = cpu_to_le16(0x9999);

	snprintf(manufacturer, sizeof(manufacturer), "%s %s with %s",
		 init_utsname()->sysname, init_utsname()->release,
		 gadget->name);

	dev = &vmidi_dev_storage;
	memset(dev, 0, sizeof(*dev));

	spin_lock_init(&dev->lock);
	dev->gadget = gadget;
	dev->in_ep = in_ep;
	dev->out_ep = out_ep;
	dev->in_port.cable = 0;
	dev->in_port.state = STATE_UNKNOWN;
	set_gadget_data(gadget, dev);

	tasklet_init(&dev->tasklet, vmidi_in_tasklet, (unsigned long)dev);

	dev->req = alloc_ep_req(gadget->ep0, USB_BUFSIZ);
	if (!dev->req) {
		vmidi_unbind(gadget);
		return -ENOMEM;
	}
	dev->req->complete = vmidi_setup_complete;

	device_desc.bMaxPacketSize0 = gadget->ep0->maxpacket;
	gadget->ep0->driver_data = dev;

	g_vmidi = dev;

	printk(KERN_INFO "vusb_midi: bind: complete, OUT %s IN %s\n",
	       EP_OUT_NAME, EP_IN_NAME);

	return 0;
}

static struct usb_gadget_driver vmidi_driver = {
	.speed     = USB_SPEED_FULL,
	.function  = (char *)longname,
	.bind      = vmidi_bind,
	.unbind    = vmidi_unbind,
	.setup     = vmidi_setup,
	.disconnect = vmidi_disconnect,
	.suspend   = vmidi_suspend,
	.resume    = vmidi_resume,
	.driver    = {
		.name  = (char *)shortname,
		.owner = THIS_MODULE,
	},
};

/*-------------------------------------------------------------------------*/
/* /proc/.vmidi interface                                                  */
/*-------------------------------------------------------------------------*/

static int vmidi_write_proc(struct file *file, const char __user *buf,
			    unsigned long count, void *data)
{
	struct vmidi_device *dev = g_vmidi;
	char kbuf[256];
	unsigned long flags;
	size_t n, i, avail;

	if (!dev || !dev->config)
		return -ENODEV;

	n = count < sizeof(kbuf) ? count : sizeof(kbuf);
	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;

	spin_lock_irqsave(&dev->lock, flags);
	avail = (MIDI_RING_SIZE - 1) - ring_count(dev);
	if (n > avail)
		n = avail;
	for (i = 0; i < n; i++) {
		dev->midi_buf[dev->midi_head] = kbuf[i];
		dev->midi_head = (dev->midi_head + 1) & MIDI_RING_MASK;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	if (n > 0)
		tasklet_hi_schedule(&dev->tasklet);

	return (int)count;
}

/*-------------------------------------------------------------------------*/
/* Direct USB device enumeration (hub driver won't bind on RTAI)           */
/*-------------------------------------------------------------------------*/

/* Enumeration is handled inside dummy_hcd.ko via module params */

/*-------------------------------------------------------------------------*/
/* Deferred init with retry (waits for dummy_hcd virtual bus)              */
/*-------------------------------------------------------------------------*/

static struct work_struct vmidi_init_work;
static int vmidi_retries;

static void vmidi_deferred_init(struct work_struct *work)
{
	int ret;

	while (vmidi_retries < 20) {
		ret = usb_gadget_register_driver(&vmidi_driver);
		if (ret == 0)
			break;
		vmidi_retries++;
		schedule();
	}
	if (ret) {
		printk(KERN_ERR "vusb_midi: gadget register failed (%d)\n",
		       ret);
		return;
	}

	printk(KERN_INFO "vusb_midi: gadget registered ok\n");

	vmidi_proc = create_proc_entry(".vmidi", 0222, NULL);
	if (vmidi_proc)
		vmidi_proc->write_proc = vmidi_write_proc;

	printk(KERN_INFO "vusb_midi: %s\n",
	       vmidi_proc ? "ready - /proc/.vmidi" : "ready (proc failed)");
}

static int vmidi_init(void)
{
	INIT_WORK(&vmidi_init_work, vmidi_deferred_init);
	schedule_work(&vmidi_init_work);
	return 0;
}

static void vmidi_exit(void)
{
	flush_scheduled_work();
	if (vmidi_proc)
		remove_proc_entry(".vmidi", NULL);
	if (g_vmidi)
		usb_gadget_unregister_driver(&vmidi_driver);
}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Virtual USB MIDI Gadget for Kronos");
module_init(vmidi_init);
module_exit(vmidi_exit);
