/* Software-based Trusted Platform Module (TPM) Emulator
 * Copyright (C) 2004-2010 Mario Strasser <mast@gmx.net>
 *
 * This module is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This module is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * $Id: tpmd_dev.c 459 2011-02-13 16:27:55Z mast $
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/slab.h>

#include <linux/socket.h>
#include <linux/net.h>
#include <linux/inet.h>
#include <linux/in.h>
#include <linux/un.h>

#include "config.h"

#define DEBUG
#define TPM_DEVICE_MINOR  224
#define TPM_DEVICE_ID     "tpm"
#define TPM_MODULE_NAME   "tpmd_dev"

#define TPM_STATE_IS_OPEN 0

#ifdef DEBUG
#define debug(fmt, ...) printk(KERN_DEBUG "%s %s:%d: Debug: " fmt "\n", \
                        TPM_MODULE_NAME, __FILE__, __LINE__, ## __VA_ARGS__)
#else
#define debug(fmt, ...)
#endif
#define info(fmt, ...)  printk(KERN_INFO "%s %s:%d: Info: " fmt "\n", \
                        TPM_MODULE_NAME, __FILE__, __LINE__, ## __VA_ARGS__)
#define error(fmt, ...) printk(KERN_ERR "%s %s:%d: Error: " fmt "\n", \
                        TPM_MODULE_NAME, __FILE__, __LINE__, ## __VA_ARGS__)
#define alert(fmt, ...) printk(KERN_ALERT "%s %s:%d: Alert: " fmt "\n", \
                        TPM_MODULE_NAME, __FILE__, __LINE__, ## __VA_ARGS__)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mario Strasser <mast@gmx.net>");
MODULE_DESCRIPTION("Trusted Platform Module (TPM) Emulator");
MODULE_SUPPORTED_DEVICE(TPM_DEVICE_ID);

/* module parameters */
char *tpmd_socket_name = TPM_SOCKET_NAME;
module_param(tpmd_socket_name, charp, 0444);
MODULE_PARM_DESC(tpmd_socket_name, " Sets the name of the TPM daemon socket.");

int tpmd_port=6543;
module_param(tpmd_port, int, 0444);
MODULE_PARM_DESC(tpmd_port, " Sets the port number of the TPM daemon socket.");
/* TPM lock */
static struct semaphore tpm_mutex;

/* TPM command response */
static struct {
  uint8_t *data;
  uint32_t size;
} tpm_response;

/* module state */
static uint32_t module_state;
static struct socket *tpmd_sock;
static struct sockaddr addr;

static int tpmd_connect(char *socket_name,int port)
{
  int res;
  struct sockaddr_in * tpm_addr;
  res = sock_create(PF_INET, SOCK_STREAM, 0, &tpmd_sock);
  if (res != 0) {
    error("sock_create() failed: %d\n", res);
    tpmd_sock = NULL;
    return res;
  }
  tpm_addr =(struct sockaddr_in *)&addr;
  tpm_addr->sin_family=AF_INET;
  tpm_addr->sin_addr.s_addr=in_aton(socket_name);
  tpm_addr->sin_port=htons(port);
  memset(&(tpm_addr->sin_zero),'\0',8);
  res = tpmd_sock->ops->connect(tpmd_sock, 
    (struct sockaddr*)&addr, sizeof(struct sockaddr), 0);
  if (res != 0) {
    error("sock_connect() failed: %d\n", res);
    tpmd_sock->ops->release(tpmd_sock);
    tpmd_sock = NULL;
    return res;
  }
  return 0;
}

static void tpmd_disconnect(void)
{
  if (tpmd_sock != NULL) tpmd_sock->ops->release(tpmd_sock);
  tpmd_sock = NULL;
}

static int tpmd_handle_command(const uint8_t *in, uint32_t in_size)
{
  int res;
  struct msghdr msg;
  struct kvec iov;

  res = tpmd_connect(tpmd_socket_name,tpmd_port);
 // open device end

  /* send command to tpmd */
  memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void*)in;
    iov.iov_len = in_size;
   res = kernel_sendmsg(tpmd_sock, &msg, &iov,1,in_size);
  if (res < 0) {
    error("sock_sendmsg() failed: %d\n", res);
    return res;
  }
  /* receive response from tpmd */
  tpm_response.size = TPM_CMD_BUF_SIZE;
  tpm_response.data = kmalloc(tpm_response.size, GFP_KERNEL);
  if (tpm_response.data == NULL) return -1;
  memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void*)tpm_response.data;
    iov.iov_len = tpm_response.size;
  res = kernel_recvmsg(tpmd_sock, &msg, &iov,1,tpm_response.size,0);
  if (res < 0) {
      error("sock_recvmsg() failed: %d\n", res);
      tpm_response.data = NULL;
    return res;
  }
  tpm_response.size = res;

// close device start
  tpmd_disconnect();
 // close device end

  return 0;
}

static int tpm_open(struct inode *inode, struct file *file)
{
  int res;
  debug("%s()", __FUNCTION__);
  if (test_and_set_bit(TPM_STATE_IS_OPEN, (void*)&module_state)) return -EBUSY;
  down(&tpm_mutex);


  res = tpmd_connect(tpmd_socket_name,tpmd_port);
  up(&tpm_mutex);
  if (res != 0) {
    clear_bit(TPM_STATE_IS_OPEN, (void*)&module_state);
    return -EIO;
  }
// close device start
  down(&tpm_mutex);
  tpmd_disconnect();
  up(&tpm_mutex);
 // close device end
  return 0;
}

static int tpm_release(struct inode *inode, struct file *file)
{
  debug("%s()", __FUNCTION__);
  down(&tpm_mutex);
  if (tpm_response.data != NULL) {
    kfree(tpm_response.data);
    tpm_response.data = NULL;
  }
  up(&tpm_mutex);
  clear_bit(TPM_STATE_IS_OPEN, (void*)&module_state);
  return 0;
}

static ssize_t tpm_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
  debug("%s(%zd)", __FUNCTION__, count);
  down(&tpm_mutex);
  if (tpm_response.data != NULL) {
    count = min(count, (size_t)tpm_response.size - (size_t)*ppos);
    count -= copy_to_user(buf, &tpm_response.data[*ppos], count);
    *ppos += count;
    if ((size_t)tpm_response.size == (size_t)*ppos) {
      kfree(tpm_response.data);
      tpm_response.data = NULL;
    }
  } else {
    count = 0;
  }
  up(&tpm_mutex);
  return count;
}

static ssize_t tpm_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
  debug("%s(%zd)", __FUNCTION__, count);
  down(&tpm_mutex);
  *ppos = 0;
  if (tpm_response.data != NULL) {
    kfree(tpm_response.data);
    tpm_response.data = NULL;
  }
  if (tpmd_handle_command(buf, count) != 0) { 
    count = -EILSEQ;
    tpm_response.data = NULL;
  }
  up(&tpm_mutex);
  return count;
}

#define TPMIOC_CANCEL   _IO('T', 0x00)
#define TPMIOC_TRANSMIT _IO('T', 0x01)

static long tpm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
  debug("%s(%d, %p)", __FUNCTION__, cmd, (char*)arg);
  if (cmd == TPMIOC_TRANSMIT) {
    uint32_t count = ntohl(*(uint32_t*)(arg + 2));
    down(&tpm_mutex);
    if (tpm_response.data != NULL) {
      kfree(tpm_response.data);
      tpm_response.data = NULL;
    }
    if (tpmd_handle_command((char*)arg, count) == 0) {
      tpm_response.size -= copy_to_user((char*)arg, tpm_response.data, tpm_response.size);
      kfree(tpm_response.data);
      tpm_response.data = NULL;
    } else {
      tpm_response.size = 0;
      tpm_response.data = NULL;
    }
    up(&tpm_mutex);
    return tpm_response.size;
  }
  return -1;
}

struct file_operations fops = {
  .owner   = THIS_MODULE,
  .open    = tpm_open,
  .release = tpm_release,
  .read    = tpm_read,
  .write   = tpm_write,
  .unlocked_ioctl = tpm_ioctl,
};

static struct miscdevice tpm_dev = {
  .minor      = TPM_DEVICE_MINOR, 
  .name       = TPM_DEVICE_ID, 
  .fops       = &fops,
};

int __init init_tpm_module(void)
{
  int res = misc_register(&tpm_dev);
  if (res != 0) {
    error("misc_register() failed for minor %d\n", TPM_DEVICE_MINOR);
    return res;
  }
  printk("tpmd_dev: input parameters %s : %d\n",tpmd_socket_name,tpmd_port);
  /* initialize variables */
  sema_init(&tpm_mutex, 1);
  module_state = 0;
  tpm_response.data = NULL;
  tpm_response.size = 0;
  tpmd_sock = NULL;
  return 0;
}

void __exit cleanup_tpm_module(void)
{
  misc_deregister(&tpm_dev);
  tpmd_disconnect();
  if (tpm_response.data != NULL) kfree(tpm_response.data);
}

module_init(init_tpm_module);
module_exit(cleanup_tpm_module);

