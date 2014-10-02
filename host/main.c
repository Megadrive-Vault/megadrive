#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <termios.h>
#include <errno.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>
#include <linux/input.h>
#include "../pkts.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

struct teensy_dev {
  int fd;
  struct {
    int ep_in;
    int ep_out;
  } ifaces[2];
};

/* return 1 if founf, 0 if not, < 0 on error */
static int find_device(struct teensy_dev *dev,
  uint16_t vendor, uint16_t product)
{
  const char path_root[] = "/dev/bus/usb";
  union {
    struct usb_descriptor_header hdr;
    struct usb_device_descriptor d;
    struct usb_config_descriptor c;
    struct usb_interface_descriptor i;
    struct usb_endpoint_descriptor e;
    char space[0x100]; /* enough? */
  } desc;
  char path_bus[256], path_dev[256];
  struct dirent *ent, *ent_bus;
  DIR *dir = NULL, *dir_bus = NULL;
  int num, fd = -1;
  int iface = -1;
  int retval = -1;
  int ret;

  memset(dev, 0xff, sizeof(*dev));

  dir = opendir(path_root);
  if (dir == NULL) {
    perror("opendir");
    return -1;
  }

  for (ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
    /* should be a number like 000 */
    if (sscanf(ent->d_name, "%03d", &num) != 1)
      continue;

    snprintf(path_bus, sizeof(path_bus), "%s/%s",
        path_root, ent->d_name);

    dir_bus = opendir(path_bus);
    if (dir_bus == NULL)
      continue;

    ent_bus = readdir(dir_bus);
    for (; ent_bus != NULL; ent_bus = readdir(dir_bus)) {
      if (sscanf(ent->d_name, "%03d", &num) != 1)
        continue;

      snprintf(path_dev, sizeof(path_dev), "%s/%s/%s",
          path_root, ent->d_name, ent_bus->d_name);

      fd = open(path_dev, O_RDWR);
      if (fd == -1)
        continue;

      ret = read(fd, &desc.d, sizeof(desc.d));
      if (ret != sizeof(desc.d)) {
        fprintf(stderr, "desc read: %d/%zd: ", ret, sizeof(desc.d));
        perror("");
        goto next;
      }

      if (desc.d.bDescriptorType != USB_DT_DEVICE) {
        fprintf(stderr, "%s: bad DT: 0x%02x\n",
            path_dev, desc.d.bDescriptorType);
        goto next;
      }

      if (desc.d.idVendor == vendor && desc.d.idProduct == product)
        goto found;

next:
      close(fd);
      fd = -1;
    }

    closedir(dir_bus);
    dir_bus = NULL;
  }

  /* not found */
  retval = 0;
  goto out;

found:
  if (desc.d.bNumConfigurations != 1) {
    fprintf(stderr, "unexpected bNumConfigurations: %u\n",
        desc.d.bNumConfigurations);
    goto out;
  }

  /* walk through all descriptors */
  while (1)
  {
    ret = read(fd, &desc.hdr, sizeof(desc.hdr));
    if (ret == 0)
      break;
    if (ret != sizeof(desc.hdr)) {
      fprintf(stderr, "desc.hdr read: %d/%zd: ", ret, sizeof(desc.hdr));
      perror("");
      break;
    }

    ret = (int)lseek(fd, -sizeof(desc.hdr), SEEK_CUR);
    if (ret == -1) {
      perror("lseek");
      break;
    }

    ret = read(fd, &desc, desc.hdr.bLength);
    if (ret != desc.hdr.bLength) {
      fprintf(stderr, "desc read: %d/%u: ", ret, desc.hdr.bLength);
      perror("");
      break;
    }

    switch (desc.hdr.bDescriptorType) {
      case USB_DT_CONFIG:
        if (desc.c.bNumInterfaces != 2) {
          fprintf(stderr, "unexpected bNumInterfaces: %u\n",
              desc.c.bNumInterfaces);
          goto out;
        }
        break;

      case USB_DT_INTERFACE:
        if (desc.i.bInterfaceClass != USB_CLASS_HID
            || desc.i.bInterfaceSubClass != 0
            || desc.i.bInterfaceProtocol != 0) {
          fprintf(stderr, "unexpected interface %x:%x:%x\n",
            desc.i.bInterfaceClass, desc.i.bInterfaceSubClass,
            desc.i.bInterfaceProtocol);
          goto out;
        }
        if (desc.i.bNumEndpoints != 2) {
          fprintf(stderr, "unexpected bNumEndpoints: %u\n",
            desc.i.bNumEndpoints);
          goto out;
        }
        iface++;
        break;

      case USB_DT_ENDPOINT:
        if (iface < 0 || iface >= ARRAY_SIZE(dev->ifaces)) {
          fprintf(stderr, "bad iface: %d\n", iface);
          goto out;
        }
        if (desc.e.wMaxPacketSize != 64 && desc.e.wMaxPacketSize != 32) {
          fprintf(stderr, "iface %d, EP %02x: "
            "unexpected wMaxPacketSize: %u\n",
            iface, desc.e.bEndpointAddress, desc.e.wMaxPacketSize);
          goto out;
        }
        if (desc.e.bEndpointAddress & 0x80)
          dev->ifaces[iface].ep_in = desc.e.bEndpointAddress; // & 0x7F;
        else
          dev->ifaces[iface].ep_out = desc.e.bEndpointAddress;
        break;

      case 0x21:
        /* ignore */
        break;

      default:
        fprintf(stderr, "skipping desc 0x%02x\n",
          desc.hdr.bDescriptorType);
        break;
    }
  }

  /* claim interfaces */
  for (iface = 0; iface < ARRAY_SIZE(dev->ifaces); iface++) {
    struct usbdevfs_ioctl usbio;

    if (dev->ifaces[iface].ep_in == -1) {
      fprintf(stderr, "missing ep_in, iface: %d\n", iface);
      goto out;
    }
    if (dev->ifaces[iface].ep_out == -1) {
      fprintf(stderr, "missing ep_out, iface: %d\n", iface);
      goto out;
    }

    /* disconnect default driver */
    memset(&usbio, 0, sizeof(usbio));
    usbio.ifno = iface;
    usbio.ioctl_code = USBDEVFS_DISCONNECT;
    ret = ioctl(fd, USBDEVFS_IOCTL, &usbio);
    if (ret != 0 && errno != ENODATA)
      perror("USBDEVFS_DISCONNECT");

    ret = ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    if (ret != 0)
      perror("USBDEVFS_CLAIMINTERFACE");
  }

  dev->fd = fd;
  fd = -1;
  retval = 1;

out:
  if (fd != -1)
    close(fd);
  if (dir_bus != NULL)
    closedir(dir_bus);
  if (dir != NULL)
    closedir(dir);

  return retval;
}

static int enable_echo(int enable)
{
  const char *portname = "/dev/tty";
  struct termios tty;
  int retval = -1;
  int ret;
  int fd;

  memset(&tty, 0, sizeof(tty));

  fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
  if (fd < 0) {
    fprintf(stderr, "open %s: ", portname);
    perror("");
    return 1;
  }

  ret = tcgetattr(fd, &tty);
  if (ret != 0) {
    perror("tcgetattr");
    goto out;
  }

  // printf("lflag: 0%o\n", tty.c_lflag);
  if (enable)
    tty.c_lflag |= ECHO;
  else
    tty.c_lflag &= ~ECHO;

  ret = tcsetattr(fd, TCSANOW, &tty);
  if (ret != 0) {
    perror("tcsetattr");
    goto out;
  }

  retval = 0;
out:
  close(fd);

  return retval;
}

static void signal_handler(int sig)
{
  enable_echo(1);
  signal(sig, SIG_DFL);
  raise(sig);
}

/* ?0SA 00DU, ?1CB RLDU */
#define STATE_BYTES 2

static uint8_t fixed_input_state[STATE_BYTES] = { 0x33, 0x3f };

enum mdbtn {
  MDBTN_UP = 1,
  MDBTN_DOWN,
  MDBTN_LEFT,
  MDBTN_RIGHT,
  MDBTN_A,
  MDBTN_B,
  MDBTN_C,
  MDBTN_START,
};

static const enum mdbtn evdev_md_map[KEY_CNT] = {
  [KEY_UP]       = MDBTN_UP,
  [KEY_DOWN]     = MDBTN_DOWN,
  [KEY_LEFT]     = MDBTN_LEFT,
  [KEY_RIGHT]    = MDBTN_RIGHT,
  [KEY_HOME]     = MDBTN_A,
  [KEY_PAGEDOWN] = MDBTN_B,
  [KEY_END]      = MDBTN_C,
  [KEY_LEFTALT]  = MDBTN_START,
};

int do_evdev_input(int fd)
{
  uint8_t old_state[STATE_BYTES];
  uint8_t changed_bits[STATE_BYTES] = { 0, };
  struct input_event ev;
  enum mdbtn mdbtn;
  int i, ret;

  ret = read(fd, &ev, sizeof(ev));
  if (ret != sizeof(ev)) {
    fprintf(stderr, "evdev read %d/%zd: ", ret, sizeof(ev));
    perror("");
    return 0;
  }

  if (ev.type != EV_KEY)
    return 0;

  if (ev.value != 0 && ev.value != 1)
    return 0;

  if ((uint32_t)ev.code >= ARRAY_SIZE(evdev_md_map)) {
    fprintf(stderr, "evdev read bad key: %u\n", ev.code);
    return 0;
  }

  mdbtn = evdev_md_map[ev.code];
  if (mdbtn == 0)
    return 0;

  memcpy(old_state, fixed_input_state, STATE_BYTES);

  /* ?0SA 00DU, ?1CB RLDU */
  switch (mdbtn) {
  case MDBTN_UP:
    changed_bits[0] = 0x01;
    changed_bits[1] = 0x01;
    break;
  case MDBTN_DOWN:
    changed_bits[0] = 0x02;
    changed_bits[1] = 0x02;
    break;
  case MDBTN_LEFT:
    changed_bits[0] = 0x00;
    changed_bits[1] = 0x04;
    break;
  case MDBTN_RIGHT:
    changed_bits[0] = 0x00;
    changed_bits[1] = 0x08;
    break;
  case MDBTN_A:
    changed_bits[0] = 0x10;
    changed_bits[1] = 0x00;
    break;
  case MDBTN_B:
    changed_bits[0] = 0x00;
    changed_bits[1] = 0x10;
    break;
  case MDBTN_C:
    changed_bits[0] = 0x00;
    changed_bits[1] = 0x20;
    break;
  case MDBTN_START:
    changed_bits[0] = 0x20;
    changed_bits[1] = 0x00;
    break;
  }

  if (ev.value) {
    // key press
    for (i = 0; i < STATE_BYTES; i++)
      fixed_input_state[i] &= ~changed_bits[i];
  }
  else {
    // key release
    for (i = 0; i < STATE_BYTES; i++)
      fixed_input_state[i] |=  changed_bits[i];
  }

  return memcmp(old_state, fixed_input_state, STATE_BYTES) ? 1 : 0;
}

struct gmv_tas {
  char sig[15];
  char ver;
  uint32_t rerecord_count;
  char ctrl1;
  char ctrl2;
  uint16_t flags;
  char name[40];
  uint8_t data[0][3];
};

static int submit_urb(int fd, struct usbdevfs_urb *urb, int ep,
  void *buf, size_t buf_size)
{
  memset(urb, 0, sizeof(*urb));
  urb->type = USBDEVFS_URB_TYPE_INTERRUPT;
  urb->endpoint = ep;
  urb->buffer = buf;
  urb->buffer_length = buf_size;

  return ioctl(fd, USBDEVFS_SUBMITURB, urb);
}

enum my_urbs {
  URB_DATA_IN,
  URB_DATA_OUT,
  URB_DBG_IN,
  URB_CNT
};

int main(int argc, char *argv[])
{
  const char *tasfn = NULL;
  struct teensy_dev dev;
  struct usbdevfs_urb urb[URB_CNT];
  struct usbdevfs_urb *reaped_urb;
  int fixed_input_changed;
  int evdev_fds[16];
  int evdev_fd_cnt = 0;
  int evdev_support;
  int wait_device = 0;
  int dbg_in_sent = 0;
  int data_in_sent = 0;
  fd_set rfds, wfds;
  struct gmv_tas *gmv = NULL;
  int enable_sent = 0;
  int frame_count = 0;
  int frames_sent = 0;
  char buf_dbg[64 + 1];
  struct tas_pkt pkt_in;
  struct tas_pkt pkt_out;
  struct timeval *timeout = NULL;
  struct timeval tout;
  int i, ret;
  int fd;

  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      if (strcmp(argv[i], "-m") != 0) {
        fprintf(stderr, "bad arg: %s\n", argv[i]);
        return 1;
      }
      i++;
      if (argv[i] == NULL) {
        fprintf(stderr, "missing arg\n");
        return 1;
      }
      tasfn = argv[i];
      continue;
    }
    if (evdev_fd_cnt >= ARRAY_SIZE(evdev_fds)) {
      fprintf(stderr, "too many evdevs\n");
      break;
    }
    fd = open(argv[i], O_RDONLY);
    if (fd == -1) {
      fprintf(stderr, "open %s: ", argv[i]);
      perror("");
      continue;
    }
    evdev_support = 0;
    ret = ioctl(fd, EVIOCGBIT(0, sizeof(evdev_support)),
                &evdev_support);
    if (ret < 0)
      perror("EVIOCGBIT");
    if (!(evdev_support & (1 << EV_KEY))) {
      fprintf(stderr, "%s doesn't have keys\n", argv[i]);
      close(fd);
      continue;
    }
    evdev_fds[evdev_fd_cnt++] = fd;
  }

  if (tasfn != NULL) {
    long size;
    FILE *f;
    
    f = fopen(tasfn, "rb");
    if (f == NULL) {
      fprintf(stderr, "fopen %s: ", tasfn);
      perror("");
      return 1;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < (long)sizeof(*gmv)) {
      fprintf(stderr, "bad gmv size: %ld\n", size);
      return 1;
    }
    gmv = malloc(size);
    if (gmv == NULL) {
      fprintf(stderr, "OOM?\n");
      return 1;
    }
    ret = fread(gmv, 1, size, f);
    if (ret != size) {
      fprintf(stderr, "fread %d/%ld: ", ret, size);
      perror("");
      return 1;
    }
    fclose(f);
    frame_count = (size - sizeof(*gmv)) / 3;

    /* check the GMV.. */
    if (frame_count <= 0 || size != sizeof(*gmv) + frame_count * 3) {
      fprintf(stderr, "broken gmv? frames=%d\n", frame_count);
      return 1;
    }

    if (strncmp(gmv->sig, "Gens Movie TEST", 15) != 0) {
      fprintf(stderr, "bad GMV sig\n");
      return 1;
    }
    if (gmv->ctrl1 != '3') {
      fprintf(stderr, "unhandled controlled config: '%c'\n", gmv->ctrl1);
      //return 1;
    }
    if (gmv->ver >= 'A') {
      if (gmv->flags & 0x40) {
        fprintf(stderr, "unhandled flag: movie requires a savestate\n");
        return 1;
      }
      if (gmv->flags & 0x20) {
        fprintf(stderr, "unhandled flag: 3-player movie\n");
        return 1;
      }
      if (gmv->flags & ~0x80) {
        fprintf(stderr, "unhandled flag(s): %04x\n", gmv->flags);
        //return 1;
      }
    }
    gmv->name[39] = 0;
    printf("loaded GMV: %s\n", gmv->name);
    printf("%d frames, %u rerecords\n",
           frame_count, gmv->rerecord_count);
  }

  enable_echo(0);
  signal(SIGINT, signal_handler);

  dev.fd = -1;

  while (1)
  {
    if (dev.fd == -1) {
      ret = find_device(&dev, 0x16C0, 0x0486);
      if (ret < 0)
        break;

      if (ret == 0) {
        if (!wait_device) {
          printf("waiting for device..\n");
          wait_device = 1;
        }
        usleep(250000);
        continue;
      }

      wait_device = 0;
      data_in_sent = 0;
      dbg_in_sent = 0;
      enable_sent = 0;
      frames_sent = 0;

      /* we wait first, then send commands, but if teensy
       * is started already, it won't send anything */
      tout.tv_sec = 1;
      tout.tv_usec = 0;
      timeout = &tout;
    }

    if (!data_in_sent) {
      memset(&pkt_in, 0, sizeof(pkt_in));
      ret = submit_urb(dev.fd, &urb[URB_DATA_IN], dev.ifaces[0].ep_in,
                       &pkt_in, sizeof(pkt_in));
      if (ret != 0) {
        perror("USBDEVFS_SUBMITURB URB_DATA_IN");
        break;
      }

      data_in_sent = 1;
    }
    if (!dbg_in_sent) {
      ret = submit_urb(dev.fd, &urb[URB_DBG_IN], dev.ifaces[1].ep_in,
                       buf_dbg, sizeof(buf_dbg) - 1);
      if (ret != 0) {
        perror("USBDEVFS_SUBMITURB URB_DBG_IN");
        break;
      }

      dbg_in_sent = 1;
    }

    FD_ZERO(&rfds);
    for (i = 0; i < evdev_fd_cnt; i++)
      FD_SET(evdev_fds[i], &rfds);

    FD_ZERO(&wfds);
    FD_SET(dev.fd, &wfds);

    ret = select(dev.fd + 1, &rfds, &wfds, NULL, timeout);
    if (ret < 0) {
      perror("select");
      break;
    }
    timeout = NULL;

    /* something from input devices? */
    fixed_input_changed = 0;
    for (i = 0; i < evdev_fd_cnt; i++) {
      if (FD_ISSET(evdev_fds[i], &rfds)) {
        fixed_input_changed |=
          do_evdev_input(evdev_fds[i]);
      }
    }

    /* something from USB? */
    if (FD_ISSET(dev.fd, &wfds))
    {
      reaped_urb = NULL;
      ret = ioctl(dev.fd, USBDEVFS_REAPURB, &reaped_urb);
      if (ret != 0) {
        if (errno == ENODEV)
          goto dev_close;
        perror("USBDEVFS_REAPURB");
        break;
      }

      if (reaped_urb != NULL && reaped_urb->status != 0) {
        errno = -reaped_urb->status;
        if ((unsigned long)(reaped_urb - urb) < ARRAY_SIZE(urb))
          fprintf(stderr, "urb #%zu: ", reaped_urb - urb);
        else
          fprintf(stderr, "unknown urb: ");
        perror("");
        if (reaped_urb->status == -EILSEQ) {
          /* this is usually a sign of disconnect.. */
          usleep(250000);
          goto dev_close;
        }
      }

      if (reaped_urb == &urb[URB_DATA_IN]) {
        /* some request from teensy */
        int count;
        uint8_t b;

        switch (pkt_in.type) {
        case PKT_STREAM_REQ:
          printf("%d/%d\n", frames_sent, frame_count);

          for (i = 0; i < sizeof(pkt_out.data); i++) {
            pkt_out.data[i * 2 + 0] = 0x33;
            pkt_out.data[i * 2 + 1] = 0x3f;
          }
          if (frames_sent < frame_count) {
            pkt_out.type = PKT_STREAM_DATA;

            count = frame_count - frames_sent;
            if (count > sizeof(pkt_out.data) / 2)
              count = sizeof(pkt_out.data) / 2;
            for (i = 0; i < count; i++) {
              /* SCBA RLDU */
              b = gmv->data[frames_sent][0];

              /* ?0SA 00DU, ?1CB RLDU */
              pkt_out.data[i * 2 + 0] = (b & 0x13) | ((b >> 2) & 0x20);
              pkt_out.data[i * 2 + 1] = (b & 0x0f) | ((b >> 1) & 0x30);

              if (gmv->data[frames_sent][1] != 0xff
                  || gmv->data[frames_sent][2] != 0xff)
              {
                fprintf(stderr, "f %d: unhandled byte(s) %02x %02x\n",
                  frames_sent, gmv->data[frames_sent][1],
                  gmv->data[frames_sent][2]);
              }

              frames_sent++;
            }
          }
          else
            pkt_out.type = PKT_STREAM_END;

          ret = submit_urb(dev.fd, &urb[URB_DATA_OUT],
                  dev.ifaces[0].ep_out, &pkt_out, sizeof(pkt_out));
          if (ret != 0)
            perror("USBDEVFS_SUBMITURB URB_DATA_OUT PKT_STREAM_DATA");
          break;

        default:
          printf("host: got unknown pkt type: %04x\n", pkt_in.type);
          break;
        }

        data_in_sent = 0;
      }
      else if (reaped_urb == &urb[URB_DATA_OUT]) {
      }
      else if (reaped_urb == &urb[URB_DBG_IN]) {
        /* debug text */
        buf_dbg[reaped_urb->actual_length] = 0;
        printf("%s", buf_dbg);
        dbg_in_sent = 0;
      }
      else {
        fprintf(stderr, "reaped unknown urb? %p #%zu\n",
          reaped_urb, reaped_urb - urb);
      }
    }

    /* something to send? */
    if (gmv != NULL && !enable_sent) {
      memset(&pkt_out, 0, sizeof(pkt_out));
      pkt_out.type = PKT_STREAM_ENABLE;
      ret = submit_urb(dev.fd, &urb[URB_DATA_OUT], dev.ifaces[0].ep_out,
                       &pkt_out, sizeof(pkt_out));
      if (ret != 0) {
        perror("USBDEVFS_SUBMITURB PKT_STREAM_ENABLE");
        continue;
      }
      enable_sent = 1;
    }
    if (gmv == NULL && fixed_input_changed) {
      memset(&pkt_out, 0, sizeof(pkt_out));
      pkt_out.type = PKT_FIXED_STATE;
      memcpy(pkt_out.data, fixed_input_state, sizeof(fixed_input_state));

      ret = submit_urb(dev.fd, &urb[URB_DATA_OUT], dev.ifaces[0].ep_out,
                       &pkt_out, sizeof(pkt_out));
      if (ret != 0) {
        perror("USBDEVFS_SUBMITURB URB_DATA_OUT");
        break;
      }
    }

    continue;

dev_close:
    close(dev.fd);
    dev.fd = -1;
  }

  enable_echo(1);

  return ret;
}

// vim: ts=2:sw=2:expandtab
