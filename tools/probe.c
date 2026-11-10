/* Print the US-122L's USB descriptors (interfaces, alt settings, endpoints). */
#include <stdio.h>
#include <libusb.h>
int main(void) {
    libusb_context *ctx; libusb_init(&ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, 0x0644, 0x800e);
    if (!h) { fprintf(stderr, "US-122L not found / cannot open\n"); return 1; }
    libusb_device *d = libusb_get_device(h);
    printf("speed %d\n", libusb_get_device_speed(d));
    struct libusb_config_descriptor *c; libusb_get_active_config_descriptor(d, &c);
    for (int i = 0; i < c->bNumInterfaces; i++)
        for (int a = 0; a < c->interface[i].num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &c->interface[i].altsetting[a];
            printf("if %d alt %d class %d eps %d\n", id->bInterfaceNumber, id->bAlternateSetting, id->bInterfaceClass, id->bNumEndpoints);
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                printf("   ep 0x%02x attr 0x%02x maxpkt %d interval %d\n", ep->bEndpointAddress, ep->bmAttributes, ep->wMaxPacketSize, ep->bInterval);
            }
        }
    libusb_free_config_descriptor(c); libusb_close(h); libusb_exit(ctx); return 0;
}
