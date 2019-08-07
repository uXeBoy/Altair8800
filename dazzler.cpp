// -----------------------------------------------------------------------------
// Altair 8800 Simulator
// Copyright (C) 2017 David Hansel
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
// -----------------------------------------------------------------------------

#include "dazzler.h"
#include "mem.h"
#include "cpucore.h"
#include "host.h"
#include "serial.h"
#include "timer.h"
#include "TurboSPI.h"

TurboSPI    g_SPI;
DigitalPin  g_PinCS, g_PinRS;
uint8_t     g_Divisor = 2; // transfer speed set to MCU's clock divide by 2

#if USE_DAZZLER==0

void dazzler_out_ctrl(byte v) {}
void dazzler_out_pict(byte v) {}
byte dazzler_in(byte port) { return 0xff; }
void dazzler_set_iface(byte iface) {}
byte dazzler_get_iface() { return 0xff; }
void dazzler_setup() {}

#else

#define DAZ_MEMBYTE   0x10
#define DAZ_FULLFRAME 0x20
#define DAZ_CTRL      0x30
#define DAZ_CTRLPIC   0x40

#define DAZ_JOY1      0x10
#define DAZ_JOY2      0x20
#define DAZ_KEY       0x30

#define DEBUGLVL 0

byte dazzler_iface = 0xff;

uint16_t dazzler_mem_start, dazzler_mem_end, dazzler_mem_size;
volatile byte d7a_port[5];

static void dazzler_send(const byte *data, uint16_t size){}

static void dazzler_send_fullframe(){}

void dazzler_write_mem_(uint16_t a, byte v)
{
  static uint8_t buffer[2048];

  a = a - dazzler_mem_start;
  buffer[a] = ((v & 0x0F) << 4 | (v & 0xF0) >> 4);

  g_PinCS.High();
  g_PinRS.Low();
  g_SPI.Send(DAZ_FULLFRAME); // VSYNC

  g_PinRS.High();
  g_PinCS.Low();
  g_SPI.Send(buffer, sizeof(buffer));
}


void dazzler_out_ctrl(byte v)
{
#if DEBUGLVL>0
  {
    static byte prev = 0xff;
    if( v!=prev ) {printf("dazzler_out_ctrl(%02x)\n", v); prev = v; }
  }
#endif

  byte b[3];
  b[0] = DAZ_CTRL;
  b[1] = v;
  dazzler_send(b, 2);

  // D7: 1=enabled, 0=disabled
  // D6-D0: bits 15-9 of dazzler memory address

  bool on = (v & 0x80)!=0;
  uint16_t a = (v & 0x7f) << 9;
  if( !on )
    {
      dazzler_mem_start = 0x0000;
      dazzler_mem_end   = 0x0000;
    }
  else if( a != dazzler_mem_start )
    {
      int n = 0, d = a-dazzler_mem_start;
      dazzler_mem_start = a;
      dazzler_mem_end   = a + dazzler_mem_size;
    }
}


void dazzler_out_pict(byte v)
{
  // D7: not used
  // D6: 1=resolution x4 (single color), 0=normal resolution (multi-color)
  // D5: 1=2k memory, 0=512byte memory
  // D4: 1=color, 0=monochrome
  // D3-D0: color info for x4 high res mode

#if DEBUGLVL>0
  {
    static byte prev = 0xff;
    if( v!=prev ) {printf("dazzler_out_pict(%02x)\n", v); prev = v; }
  }
#endif

  byte b[2];
  b[0] = DAZ_CTRLPIC;
  b[1] = v;
  dazzler_send(b, 2);

  uint16_t s = v & 0x20 ? 2048 : 512;
  if( s > dazzler_mem_size )
    {
      dazzler_mem_size = s;
      dazzler_send_fullframe();
    }
  else
    dazzler_mem_size = s;

  dazzler_mem_end  = dazzler_mem_start + s;
}


inline void set_d7a_port(byte p, byte v)
{
#if DEBUGLVL>0
  printf("set_d7a_port(%i, %02x)\n", 0030+p, v);
#endif
  d7a_port[p]=v;
}


void dazzler_receive(byte iface, byte data)
{
  static byte state=0;

#if DEBUGLVL>0
  Serial.print("dazzler_receive: "); Serial.println(data, HEX);
#endif

  switch( state )
    {
    case 0:
      {
        state = data & 0xf0;
        switch( state )
          {
          case DAZ_JOY1:
            set_d7a_port(0, (d7a_port[0] & 0xF0) | (data & 0x0F));
            break;

          case DAZ_JOY2:
            set_d7a_port(0, (d7a_port[0] & 0x0F) | ((data & 0x0F)*16));
            break;

          case DAZ_KEY:
            break;

          default:
            state = 0;
          }
        break;
      }

    case DAZ_JOY1:
      set_d7a_port(1, data);
      state++;
      break;

    case DAZ_JOY1+1:
      set_d7a_port(2, data);
      state = 0;
      break;

    case DAZ_JOY2:
      set_d7a_port(3, data);
      state++;
      break;

    case DAZ_JOY2+1:
      set_d7a_port(4, data);
      state = 0;
      break;

    case DAZ_KEY:
      int i = config_serial_map_sim_to_host(CSM_SIO);
      if( i<0xff ) serial_receive_host_data(i, data);
      state = 0;
      break;
    }
}


byte dazzler_in(byte port)
{
  byte v = 0;

  if( port==0016 )
    {
      // the values here are approximated and certainly not
      // synchronized with the actual picture on the client
      // we just provide them here so programs waiting for the
      // signals don't get stuck.
      // If I understand correctly, the Dazzler does not
      // interlace two fields and instead counts each field
      // (half-frame) as a full frame (of 262 lines). Therefore
      // its frame rate is 29.97 * 2 = 59.94Hz, i.e. 16683us per frame,
      // i.e. 33367 cycles per frame.
      const uint32_t cycles_per_frame = 33367;
      const uint32_t cycles_per_line  = cycles_per_frame/262;

      // determine position within frame
      uint32_t c = timer_get_cycles() % cycles_per_frame;

      // bits 0-5 are unused
      v = 0xff;

      // bit 6: is low for 4ms (8000 cycles) between frames
      // (we pull it low at the beginning of a frame)
      // NOTE: bit 7 is also low during this period
      if( c<8000 ) v &= ~0xC0;

      // bit 7: low for odd line, high for even line
      if( (c/cycles_per_line)&1 ) v &= ~0x80;
    }
  else if( port>=0030 && port<0035 )
    {
      // D+7A I/O board
      // The D+7A board was not part of the Dazzler but we include
      // it in the Dazzler emulation to support joysticks
      v = d7a_port[port-0030];
    }

#if DEBUGLVL>1
  printf("%04x: dazzler_in(%i)=%02x\n", regPC, port, v);
#endif

  return v;
}


void dazzler_set_iface(byte iface)
{
  static host_serial_receive_callback_tp fprev = NULL;

  if( iface != dazzler_iface )
    {
      // if we had an interface set, restore that interface's previous receive callback
      if( dazzler_iface<0xff ) host_serial_set_receive_callback(dazzler_iface, fprev);

      // set receive callback
      dazzler_iface = iface;
      fprev = host_serial_set_receive_callback(dazzler_iface, dazzler_receive);

#if DEBUGLVL>0
      if( iface==0xff ) Serial.println("Dazzler disabled"); else {Serial.print("Dazzler on interface:"); Serial.println(iface);}
#endif
    }
}


byte dazzler_get_iface()
{
  return dazzler_iface;
}


void dazzler_setup()
{
  // setup pins
  g_PinCS.Begin(9);
  g_PinRS.Begin(8);

  g_PinCS.PinMode(OUTPUT);
  g_PinRS.PinMode(OUTPUT);

  // setup SPI
  g_SPI.Begin();
  g_SPI.Init(g_Divisor);

  dazzler_mem_start = 0x0000;
  dazzler_mem_end   = 0x0000;
  dazzler_mem_size  = 512;
  for(int i=0; i<5; i++) d7a_port[i]=0xff;

  dazzler_set_iface(config_dazzler_interface());
}

#endif
