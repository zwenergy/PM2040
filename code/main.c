//#define MULTICART

#include "pico/stdlib.h"

#ifndef MULTICART
#include "rom.h"
#else
#include "multirom.h"
#include "multimenu.h"
// For 16 MB Flash IC
//#include "multimenu_20slots.h"
#endif

#include "hardware/clocks.h"

#include "hardware/vreg.h"

#include "hardware/pio.h"
#include "hardware/dma.h"

#include "oe.pio.h"
#include "pushData.pio.h"
#include "hale.pio.h"

#ifdef MULTICART
#include "writecheck.pio.h"
#include "writecheck_addr.pio.h"
#include "lale_menu.pio.h"
#include "lale_512k.pio.h"

#define DELAY 100000
#define ROMSIZE 524288

#else
#include "lale.pio.h"
#endif


// We don't use the Flash cache.
#define XIP_CACHE   0x10000000
#define XIP_NOCACHE 0x13000000
#define XIP_NOCACHE_OFFSET (XIP_NOCACHE - XIP_CACHE)

// Pin Definitions.
#define A0A10 0
#define A1A11 1
#define A2A12 2
#define A3A13 3
#define A4A14 4
#define A5A15 5
#define A6A16 6
#define A7A17 7
#define A8A18 8
#define A9A19 9
#define A20 10

#define D0 17
#define D1 18
#define D2 19
#define D3 20
#define D4 21
#define D5 22
#define D6 23
#define D7 24

#define HALE 11
#define LALE 12
#define WE 13
#define OE 14
#define CS 15
#define CART_IRQ 26

void __not_in_flash_func( doPIOStuff() ) {
  // Set up PIOs.
  
  // OE toggle program.
  PIO pio = pio0;
  uint sm_oe = pio_claim_unused_sm( pio, false );
  uint offset_oe = pio_add_program( pio, &oe_toggle_program );
  
  // Push byte out.
  uint sm_pushData = pio_claim_unused_sm( pio, false );
  uint offset_pushData = pio_add_program( pio, &push_databits_program );
  
  // HALE latching.
  uint sm_hale = pio_claim_unused_sm( pio, false );
  uint offset_hale = pio_add_program( pio, &hale_latch_program );
  
  // LALE latching.
  uint sm_lale = pio_claim_unused_sm( pio, false );
  #ifndef MULTICART
  uint offset_lale = pio_add_program( pio, &lale_latch_program );
  #else
  uint offset_lale = pio_add_program( pio, &lale_latch_menu_program );
  #endif
  
  
  // Create DMAs.
  int hale_dma = dma_claim_unused_channel( true );
  int lale_addr_dma = dma_claim_unused_channel( true );
  int data_dma = dma_claim_unused_channel( true );
  
  
  // Move high address to LALE SM.
  dma_channel_config c = dma_channel_get_default_config( hale_dma );

  channel_config_set_transfer_data_size( &c, DMA_SIZE_32 );
  channel_config_set_read_increment( &c, false );
  channel_config_set_write_increment( &c, false );
  channel_config_set_dreq( &c, pio_get_dreq( pio, sm_hale, false) );

  dma_channel_configure(
    hale_dma,
    &c,
    &pio->txf[ sm_lale ], // Write to the LALE SM
    &pio->rxf[ sm_hale ],  // Read from HALE RX FIFO
    1,                                          // Halt after each read
    false                                       // Don't start yet
  );
  
  // Move the adress from LALE SM to the third DMA channel.
  c = dma_channel_get_default_config( lale_addr_dma );

  channel_config_set_transfer_data_size( &c, DMA_SIZE_32 );
  channel_config_set_read_increment( &c, false );
  channel_config_set_write_increment( &c, false );
  channel_config_set_dreq( &c, pio_get_dreq( pio, sm_lale, false) );
  
  channel_config_set_chain_to( &c, hale_dma );     // Trigger the HALE channel again when done
  
  

  dma_channel_configure(
    lale_addr_dma,
    &c,
    &dma_hw->ch[ data_dma ].al3_read_addr_trig, // Write to READ_ADDR_TRIG of data channel
    &pio->rxf[ sm_lale ], // Read from LALE RX FIFO
    1,                                          // Halt after each read
    false                                       // Don't start yet
  );
  
  
  // Read the actual data.
  c = dma_channel_get_default_config( data_dma );

  channel_config_set_transfer_data_size( &c, DMA_SIZE_8 );
  channel_config_set_read_increment( &c, false );
  channel_config_set_write_increment( &c, false );
  channel_config_set_chain_to( &c, lale_addr_dma );     // Trigger the LALE channel again when done
  
  // Set to high priority.
  channel_config_set_high_priority( &c, true );

  dma_channel_configure(
    data_dma,
    &c,
    &pio->txf[ sm_pushData ], // Write to the byte push SM
    &rom[0], // Read from ROM array (will be overwritten)
    1,                                          // Halt after each read
    false                                       // Don't start yet
  );
  
  // Start the SMs.
  oe_toggle_program_init( pio, sm_oe, offset_oe, D0, OE );
  push_databits_program_init( pio, sm_pushData, offset_pushData, D0 );
  hale_latch_program_init( pio, sm_hale, offset_hale, A0A10, HALE );
  #ifndef MULTICART
  lale_latch_program_init( pio, sm_lale, offset_lale, A0A10, LALE );
  #else
  lale_latch_menu_program_init( pio, sm_lale, offset_lale, A0A10, LALE );
  #endif
  
  // Push the base address of the array.
  #ifndef MULTICART
  pio_sm_put( pio, sm_lale, ( ( (uint32_t) rom + XIP_NOCACHE_OFFSET ) ) >> 20 );
  #else
  pio_sm_put( pio, sm_lale, ( ( (uint32_t) rom_menu + XIP_NOCACHE_OFFSET ) ) >> 14 );
  #endif
  
  // Start the DMA channels.
  dma_start_channel_mask( 1u << hale_dma );
  dma_start_channel_mask( 1u << lale_addr_dma );

  #ifdef MULTICART
  // Wait a bit.
  for ( uint32_t cnt = 0; cnt < DELAY; ++cnt ) {
    tight_loop_contents();
  }

  // Now also start the write check PIO.
  PIO pioWE = pio1;
  uint sm_we = pio_claim_unused_sm( pioWE, false );
  uint offset_we = pio_add_program( pioWE, &write_check_program );
  write_check_program_init( pioWE, sm_we, offset_we, D0, WE );
  
  // Start the write addres SM.
  uint sm_we_addr = pio_claim_unused_sm( pioWE, false );
  uint offset_we_addr = pio_add_program( pioWE, &write_check_addr_program );
  write_check_addr_program_init( pioWE, sm_we_addr, offset_we_addr, A0A10, WE );
  
  // Wait till proper write.
  uint32_t writeData;
  uint32_t addrData;
  while ( 1 ) {
    if ( !pio_sm_is_rx_fifo_empty( pioWE, sm_we ) ) {
      // Got a write. Right lower address?
      writeData = pio_sm_get( pioWE, sm_we );
      addrData = pio_sm_get( pioWE, sm_we_addr );
      
      if ( addrData == 0x3FF ) {
        // Fitting lower address.
        break;
      }
      
    }
  }
  
  uint32_t romAddress = (uint32_t) rom;
  romAddress += (ROMSIZE * writeData);
  
  // Stop the LALE SM.
  pio_sm_set_enabled( pio, sm_lale, false );
  
  // Remove the old program.
  pio_remove_program( pio, &lale_latch_menu_program, offset_lale );
  
  // Add the new program at the same offset.
  pio_add_program_at_offset( pio, &lale_latch_program, offset_lale );
  
  // Restart the LALE SM.
  lale_latch_program_init( pio, sm_lale, offset_lale, A0A10, LALE );
  
  // Add the new ROM address.
  pio_sm_put( pio, sm_lale, ( ( romAddress + XIP_NOCACHE_OFFSET ) ) >> 19 );
  
  // Stop WE checking SMs.
  pio_sm_set_enabled( pioWE, sm_we, false );
  pio_sm_set_enabled( pioWE, sm_we_addr, false );
  #endif

  // Example for the cart irq pin. We simply flip the cart irq signal
  // every 5 seconds as a very basic timer.
  gpio_init( CART_IRQ );
  gpio_set_dir( CART_IRQ, GPIO_OUT );
  gpio_put( CART_IRQ, 0 );
  
  // Do nothing but toggling the cart irq.
  while ( 1 ) {
    sleep_ms( 5000 );
    gpio_put( CART_IRQ, 1 );
    sleep_ms( 5000 );
    gpio_put( CART_IRQ, 0 );
  }
}

int main() {
  
  // Set higher freq.
  sleep_ms(2);
  vreg_set_voltage(VREG_VOLTAGE_1_30);
  sleep_ms(2);
  set_sys_clock_khz(240000, true);
  
  doPIOStuff();
  
  return 0;
}
