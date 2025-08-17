// This is a simple proof of concept for a program running on the RP2040
// and creating the actual display data for the Pokemon mini.
// The program contained in "rpfb.h" runs on the Pokemon mini and pulls
// the data from the "RP2040 framebuffer" into the LCD controller
// GDRAM. After this copying, the Pokemon mini gets the button states
// and sends it over to the RP2040 cart (at the address 0x5000).
// No sound for now. The PM program copies the frame buffer at a rate of
// 32 Hz.

#include "pico/stdlib.h"
#include <string.h>

#include "rpfb.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "hardware/pio.h"
#include "hardware/dma.h"

#include "oe.pio.h"
#include "pushData.pio.h"
#include "hale.pio.h"
#include "lale.pio.h"

#include "writecheck.pio.h"
#include "writecheck_addr.pio.h"

// Packed framebuffer (which is copied from the cart)
#define FBOFFSET 0x4000

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

#define DWIDTH 96
#define DHEIGHT 64

#define PADDLEW 4
#define PADDLEH 13

#define PADDLE1X 3
#define PADDLE1Y ( ( DHEIGHT / 2 ) - ( PADDLEH /2 ) )
#define PADDLE2X ( DWIDTH - PADDLEW - 1 )
#define BALLW 3
#define BALLX ( ( DWIDTH / 2 ) - ( BALLW / 2 ) )
#define BALLY ( ( DHEIGHT / 2 ) - ( BALLW / 2 ) )


// "Unpacked" framebuffer
uint8_t fbFull[ 96 * 64];

void __not_in_flash_func( drawRectangle )( unsigned int x, unsigned int y, 
  unsigned int lenX, unsigned int lenY ) {
  for ( unsigned int xi = x; xi < x + lenX; ++xi ) {
    fbFull[ xi + DWIDTH * y ] = 1;
    fbFull[ xi + DWIDTH * ( y + lenY - 1) ] = 1;
  }
  
  for ( unsigned int yi = y; yi < y + lenY; ++yi ) {
    fbFull[ x + DWIDTH * yi ] = 1;
    fbFull[ x + lenX - 1 + DWIDTH * yi ] = 1;
  }
}

// Actual program running and filling the FB.
void __not_in_flash_func( fbApp )( PIO wrPIO, uint sm_we, uint sm_we_addr ) {
  // Frame buffer setup.
  uint8_t* fb = rom + FBOFFSET;

  uint32_t keys = 0;
  
  // Paddle pos.
  uint32_t p1y = PADDLE1Y;
  uint32_t p2y = PADDLE1Y;
  
  // Ballpos.
  int32_t bx = BALLX;
  int32_t by = BALLY;
  
  // Ball movement.
  int32_t bspeedX = 1;
  int32_t bspeedY = 1;
  
  while ( 1 ) {
    
    // Scan keys.
    if ( keys & 0b00001000 ) {
      // Up.
      if ( p1y > 0 ) {
        --p1y;
      }
    }
    
    if ( keys & 0b00010000 ) {
      // Down.
      if ( p1y < ( 63 - PADDLEH ) ) {
        ++p1y;
      }
    }
    
    if ( keys & 0b00000100 ) {
      // C.
      if ( p2y > 0 ) {
        --p2y;
      }
    }
    
    if ( keys & 0b00000001 ) {
      // A.
      if ( p2y < ( 63 - PADDLEH ) ) {
        ++p2y;
      }
    }
    
    // Move the ball.
    int32_t potX = bx + bspeedX;
    int32_t potY = by + bspeedY;
    
    if ( potY < DHEIGHT - BALLW &&
         potY >= 0 ) {
      by = potY;
      
    } else {
      bspeedY = -bspeedY;
    }
    
    if ( potX <= PADDLE1X + PADDLEW && bspeedX < 0 ) {
      // Collision?
      if ( potY >= p1y && potY < p1y + PADDLEH ) {
        bspeedX = -bspeedX;
        
      } else if ( potX <= PADDLEW ) {
        bx = BALLX;
        by = BALLY;
        
      } else {
        bx = potX;
      }
      
    } else if ( potX >= PADDLE2X - BALLW && bspeedX > 0 ) {
      // Collision?
      if ( potY >= p2y && potY < p2y + PADDLEH ) {
        bspeedX = -bspeedX;
        
      } else if ( potX >= PADDLE2X + PADDLEW ) {
        bx = BALLX;
        by = BALLY;
        
      } else {
        bx = potX;
      }
      
    } else {
      bx = potX;
    }
    
    
    // Start from scratch.
    memset( fbFull, 0, DWIDTH * DHEIGHT );
    
    // Draw players.
    drawRectangle( PADDLE1X, p1y, PADDLEW, PADDLEH );
    drawRectangle( PADDLE2X, p2y, PADDLEW, PADDLEH );
    drawRectangle( bx, by, BALLW, BALLW );
    
    // Transform full frameBuffer.
    for ( unsigned int y = 0; y < 8; ++y ) {
      for ( unsigned int x = 0; x < 96; ++x ) {
        unsigned tmp = 0;
        for ( unsigned int i = 0; i < 8; ++i ) {
          tmp |= ( fbFull[ x + y * 96 * 8 + i * 96 ] & 1 ) << i;
        }
        
        fb[ x + y * 96 ] = tmp;
      }
    }
    
    // The cart sends the current keys directly after copying the frame buffer.
    // We can use that for syncing.
    
    uint32_t addrData;
    while ( 1 ) {
      if ( !pio_sm_is_rx_fifo_empty( wrPIO, sm_we ) ) {
        // Got a write. Right lower address?
        keys = pio_sm_get( wrPIO, sm_we );
        addrData = pio_sm_get( wrPIO, sm_we_addr );
        
        if ( addrData == 0x000 ) {
          // Fitting lower address.
          break;
        }
        
      }
    }
    
  }
}

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
  uint offset_lale = pio_add_program( pio, &lale_latch_program );
  
  
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
  pio_sm_put( pio, sm_lale, ( ( (uint32_t) rom ) ) >> 15 );
  
  // Start the DMA channels.
  dma_start_channel_mask( 1u << hale_dma );
  dma_start_channel_mask( 1u << lale_addr_dma );

  // Now also start the write check PIO.
  PIO pioWE = pio1;
  uint sm_we = pio_claim_unused_sm( pioWE, false );
  uint offset_we = pio_add_program( pioWE, &write_check_program );
  write_check_program_init( pioWE, sm_we, offset_we, D0, WE );
  
  // Start the write addres SM.
  uint sm_we_addr = pio_claim_unused_sm( pioWE, false );
  uint offset_we_addr = pio_add_program( pioWE, &write_check_addr_program );
  write_check_addr_program_init( pioWE, sm_we_addr, offset_we_addr, A0A10, WE );

  // Execute the code which handles the framebuffer.
  fbApp( pioWE, sm_we, sm_we_addr );
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
