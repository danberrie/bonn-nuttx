/*
 * Copyright (C) 2015 Motorola Mobility, LLC.
 * Copyright (c) 2014-2015 Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <nuttx/config.h>
#include <nuttx/greybus/greybus.h>
#include <nuttx/list.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arch/board/slice.h>

#include <apps/greybus-utils/manifest.h>
#include <apps/greybus-utils/svc.h>
#include <apps/greybus-utils/utils.h>

#include "bus.h"
#include "bus-i2c.h"
#include "cmd_main.h"

static struct slice_bus_data bus_data;

void bus_interrupt(struct slice_bus_data *slf, uint8_t int_mask, bool assert)
{
  if (assert)
    slf->reg_int |= int_mask;
  else
    slf->reg_int &= ~(int_mask);

  slice_host_int_set(slf->reg_int > 0);
}

void bus_greybus_from_base(struct slice_bus_data *slf, size_t len)
{
  struct slice_unipro_msg_rx *umsg;
  int i;
  uint8_t checksum;

  umsg = (struct slice_unipro_msg_rx *) slf->reg_unipro_rx;

  /*
   * Check the checksum if the checksum is non-zero. A zero checksum signals
   * the base does not request a checksum check (useful for debugging when
   * sending raw messages manually).
   */
  if (umsg->checksum != 0)
    {
      for (i = 0, checksum = 0; i < len; ++i)
        {
          checksum += slf->reg_unipro_rx[i];
        }

      if (checksum)
        {
          logd("Checksum error! Ignoring message.\n");
          gb_dump(slf->reg_unipro_rx, len);
          return;
        }
    }

  if (umsg->bundle_cport < SLICE_NUM_CPORTS)
    {
      /* Save base cport so response can be sent back correctly */
      slf->to_base_cport[umsg->bundle_cport] = umsg->hd_cport;

      greybus_rx_handler(umsg->bundle_cport, umsg->data, len - 3);
    }
  else
    logd("Invalid cport number\n");
}

static inline uint8_t calc_checksum(uint8_t *data, size_t len)
{
  uint8_t chksum = 0;
  int i;

  // Calculate the checksum
  for (i = 0; i < len; i++)
      chksum += data[i];
  return ~chksum + 1;
}

int bus_greybus_to_base(unsigned int cportid, const void *buf, size_t len)
{
  struct slice_tx_msg *m;
  struct slice_unipro_msg_tx *umsg;

  m = malloc(sizeof(struct slice_tx_msg));
  if (!m)
      return -ENOMEM;

  m->size = len + sizeof(struct slice_unipro_msg_tx);
  umsg = malloc(m->size);
  if (!umsg)
    {
      free(m);
      return -ENOMEM;
    }
  m->buf = (uint8_t *)umsg;

  umsg->checksum = 0;
  umsg->hd_cport = bus_data.to_base_cport[cportid];
  memcpy(umsg->data, buf, len);
  umsg->checksum = calc_checksum((uint8_t *)umsg, m->size);

  logd("bundle_cport=%d, hd_cport=%d, len=%d, m->size=%d, fifo_empty=%d\n",
       cportid, bus_data.to_base_cport[cportid], len, m->size,
       list_is_empty(&bus_data.reg_unipro_tx_fifo));
  gb_dump(umsg->data, len);

  list_add(&bus_data.reg_unipro_tx_fifo, &m->list);
  bus_interrupt(&bus_data, SLICE_REG_INT_UNIPRO, true);

  return 0;
}

int bus_svc_to_base(void *buf, size_t length)
{
  struct slice_tx_msg *m;

  bool was_empty = list_is_empty(&bus_data.reg_svc_tx_fifo);
  logd("length=%d, fifo_empty=%d\n", length, was_empty);

  m = malloc(sizeof(struct slice_tx_msg));
  if (m)
    {
      m->buf = malloc(length);
      if (m->buf)
        {
          memcpy(m->buf, buf, length);
          m->size = length;
          list_add(&bus_data.reg_svc_tx_fifo, &m->list);

          bus_interrupt(&bus_data, SLICE_REG_INT_SVC, true);
          return 0;
        }
      else
        {
          free(m);
        }
    }

  return -1;
}

int bus_init(void)
{
  list_init(&bus_data.reg_svc_tx_fifo);
  list_init(&bus_data.reg_unipro_tx_fifo);

  return bus_i2c_init(&bus_data);
}

static void bus_cleanup_list(struct list_head *head)
{
  struct list_head *iter;
  struct list_head *iter_next;
  struct slice_tx_msg *m;

  list_foreach_safe(head, iter, iter_next)
    {
      m = list_entry(iter, struct slice_tx_msg, list);
      list_del(iter);
      free(m->buf);
      free(m);
    }
}

void bus_cleanup(void)
{
  // Deassert interrupt line
  bus_interrupt(&bus_data, SLICE_REG_INT_SVC | SLICE_REG_INT_UNIPRO, false);

  // Drop all Unipro messages (if any)
  bus_cleanup_list(&bus_data.reg_unipro_tx_fifo);

  // Drop all SVC messages (if any)
  bus_cleanup_list(&bus_data.reg_svc_tx_fifo);
}
