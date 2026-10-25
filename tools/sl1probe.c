/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sl1probe - read-only: finds the SL1 and prints its interfaces, endpoints
 * and audio formats. Never claims interfaces and never writes to the device.
 */
#include <libusb.h>
#include <stdio.h>

#include "../src/sl1_proto.h"

static const char *xfer_type(uint8_t attr)
{
    static const char *t[] = { "control", "isochronous", "bulk", "interrupt" };
    return t[attr & 3];
}

/* Class-specific AS format type descriptor (UAC1 FORMAT_TYPE_I). Subtype 0x02
 * means FORMAT_TYPE only on streaming interfaces (subclass 2); on control
 * interfaces it is an input terminal. */
static void print_extra(const unsigned char *p, int len, int subclass)
{
    while (len >= 2 && p[0] >= 2 && p[0] <= len) {
        if (subclass == 2 && p[1] == 0x24 && p[2] == 0x02 && p[0] >= 8 && p[3] == 0x01) {
            printf("      format: PCM, %u ch, %u bit, rates:", p[4], p[6]);
            for (int i = 0; i < p[7] && 8 + 3 * i + 2 < p[0]; i++)
                printf(" %u", p[8 + 3 * i] | p[9 + 3 * i] << 8 | p[10 + 3 * i] << 16);
            printf(" Hz\n");
        } else if (p[1] == 0x21) {
            printf("      HID-style descriptor, report descriptor %u bytes\n", p[7] | p[8] << 8);
        }
        len -= p[0];
        p += p[0];
    }
}

int main(void)
{
    libusb_context *ctx;
    if (libusb_init_context(&ctx, NULL, 0)) {
        fprintf(stderr, "libusb_init failed\n");
        return 1;
    }
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int found = 0;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) || dd.idVendor != SL1_VID ||
            dd.idProduct != SL1_PID)
            continue;
        found = 1;
        printf("SL1 found: bus %u address %u, USB %x.%02x, speed %d (2 = full speed)\n",
               libusb_get_bus_number(list[i]), libusb_get_device_address(list[i]),
               dd.bcdUSB >> 8, dd.bcdUSB & 0xff, libusb_get_device_speed(list[i]));
        struct libusb_config_descriptor *cfg;
        if (libusb_get_config_descriptor(list[i], 0, &cfg)) {
            printf("  cannot read configuration descriptor\n");
            continue;
        }
        printf("  configuration %u, %u interfaces\n", cfg->bConfigurationValue,
               cfg->bNumInterfaces);
        for (int k = 0; k < cfg->bNumInterfaces; k++) {
            const struct libusb_interface *itf = &cfg->interface[k];
            for (int a = 0; a < itf->num_altsetting; a++) {
                const struct libusb_interface_descriptor *d = &itf->altsetting[a];
                printf("  interface %u alt %u: class %02x subclass %02x, %u endpoint(s)\n",
                       d->bInterfaceNumber, d->bAlternateSetting, d->bInterfaceClass,
                       d->bInterfaceSubClass, d->bNumEndpoints);
                print_extra(d->extra, d->extra_length, d->bInterfaceSubClass);
                for (int e = 0; e < d->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ep = &d->endpoint[e];
                    printf("    endpoint 0x%02x %s %s, max %u bytes, interval %u\n",
                           ep->bEndpointAddress, ep->bEndpointAddress & 0x80 ? "IN " : "OUT",
                           xfer_type(ep->bmAttributes), ep->wMaxPacketSize, ep->bInterval);
                }
            }
        }
        libusb_free_config_descriptor(cfg);

        libusb_device_handle *h;
        int rc = libusb_open(list[i], &h);
        if (rc) {
            printf("  open for streaming: NOT possible (%s)%s\n", libusb_strerror(rc),
                   rc == LIBUSB_ERROR_ACCESS ? " - install udev/50-opensl1.rules" : "");
        } else {
            printf("  open for streaming: possible\n");
            for (int k = 0; k < SL1_NUM_INTERFACES; k++)
                if (libusb_kernel_driver_active(h, k) == 1)
                    printf("    interface %d currently bound to a kernel driver\n", k);
            libusb_close(h);
        }
    }
    if (!found)
        printf("no SL1 (%04x:%04x) found\n", SL1_VID, SL1_PID);
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return found ? 0 : 1;
}
