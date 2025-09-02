// This is a simple proof of concept for a program running on the RP2040
// and creating the actual display data for the Pokemon mini.
// The program contained in "rpfb.h" runs on the Pokemon mini and pulls
// the data from the "RP2040 framebuffer" into the LCD controller
// GDRAM. After this copying, the Pokemon mini gets the button states
// and sends it over to the RP2040 cart (at the address 0x5000).
// No sound for now.

#include "pico/stdlib.h"
#include <string.h>

#include "rpfb.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "hardware/pio.h"
#include "hardware/dma.h"

#include "pico/multicore.h"

#include "oe.pio.h"
#include "pushData.pio.h"
#include "hale.pio.h"
#include "lale.pio.h"

#include "writecheck.pio.h"
#include "writecheck_addr.pio.h"

// Packed framebuffer (which is copied from the cart)
#define FBOFFSET 0x4000

// Rumble address
#define RUMBLE (rom + 0x4300)

// Rumble on.
#define RUMBLEON *( (volatile uint8_t*) RUMBLE ) = 1

// Rumble off.
#define RUMBLEOFF *( (volatile uint8_t*) RUMBLE ) = 0

// FRAMEBLEND defines the number of colors and the number of frames
// blended together to create these colors.
// FRAMEBLEND == 0: B/W, 2 colors, no frames being blended.
// FRAMEBLEND == 1: 3 colors, 2 frames being blended.
// etc.
// The PM display refreshes at roughly 75 Hz, FRAMEBLEND == 1 will reduce
// this effectively to the half and so on.
#define FRAMEBLEND 1
volatile unsigned frameBlendCnt = 0;

// PIO-related variables.
PIO pioWE_g;
uint sm_we_g, sm_we_addr_g;

// PM buttons.
volatile uint32_t pmKeys = 0;

// PM FRAME DONE notify. Is set to 1 when a "complete" frame including
// frame blending was drawn.
volatile uint32_t frameDone = 0;

// TRIPLEFB enables triple frame buffer to avoid screen tearing.
// The "frameDone" notify can also be used to sync to reduce latency 
// and do not use the triple FB.
#define TRIPLEFB

#ifdef TRIPLEFB
#define FBDEPTH 3
volatile uint32_t readPtr = 0;
volatile uint32_t writePtr = 1;
uint64_t frameTimeUS = 26667;
#else
#define FBDEPTH 1
#endif

// "Unpacked" framebuffer
uint8_t fbFull[ FBDEPTH ][ 96 * 64];


//////// PM2040 FB COPY SPECIFIC STUFF END //////////////


// Rumble frames.
#define RUMBLEFRAMES 5

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

void __not_in_flash_func( drawRectangle )( unsigned int x, unsigned int y, 
  unsigned int lenX, unsigned int lenY ) {
    
  uint8_t* curFB;
  #ifdef TRIPLEFB
  curFB = ( (uint8_t*) fbFull ) + ( writePtr * 96 * 64 );
  #else
  curFB = ( (uint8_t*) fbFull );
  #endif
  
  for ( unsigned int xi = x; xi < x + lenX; ++xi ) {    
    curFB[ xi + DWIDTH * y ] = 0b11;
    curFB[ xi + DWIDTH * ( y + lenY - 1) ] = 0b11;
  }
  
  for ( unsigned int yi = y; yi < y + lenY; ++yi ) {
    curFB[ x + DWIDTH * yi ] = 0b11;
    curFB[ x + lenX - 1 + DWIDTH * yi ] = 0b11;
  }
}

void __not_in_flash_func( drawVertLine )( unsigned int x, unsigned int y, 
  unsigned int len, unsigned int color ) {
    
  uint8_t* curFB;
  #ifdef TRIPLEFB
  curFB = ( (uint8_t*) fbFull ) + ( writePtr * 96 * 64 );
  #else
  curFB = ( (uint8_t*) fbFull );
  #endif
  
  for ( unsigned int i = y; i < y + len; ++i ) {
    curFB[ x + DWIDTH * i ] = color;
  }
}

uint32_t rumbleFrames = 0;
// Actual program running and filling the FB.
void __not_in_flash_func( doPongify )() {
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
    #ifdef TRIPLEFB
    // Time frame.
    uint64_t frameStart = time_us_64();
    #endif
    
    // Turn rumble off again.
    if ( rumbleFrames > 0 ) {
      --rumbleFrames;
      RUMBLEON;
    } else {
      RUMBLEOFF;
    }
    
    // Scan keys.
    if ( pmKeys & 0b00001000 ) {
      // Up.
      if ( p1y > 0 ) {
        --p1y;
      }
    }
    
    if ( pmKeys & 0b00010000 ) {
      // Down.
      if ( p1y < ( 63 - PADDLEH ) ) {
        ++p1y;
      }
    }
    
    if ( pmKeys & 0b00000100 ) {
      // C.
      if ( p2y > 0 ) {
        --p2y;
      }
    }
    
    if ( pmKeys & 0b00000001 ) {
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
        
        // Turn on rumble for one frame.
        RUMBLEON;
        rumbleFrames = RUMBLEFRAMES;
        
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
        
        // Turn on rumble for one frame.
        RUMBLEON;
        rumbleFrames = RUMBLEFRAMES;
        
      } else if ( potX >= PADDLE2X + PADDLEW ) {
        bx = BALLX;
        by = BALLY;
        
      } else {
        bx = potX;
      }
      
    } else {
      bx = potX;
    }
    
    #ifdef TRIPLEFB
    // Advance write pointer?
    uint32_t nextWritePtr = writePtr + 1;
    if ( nextWritePtr == 3 ) {
      nextWritePtr = 0;
    }
    
    if ( nextWritePtr != readPtr ) {
      writePtr = nextWritePtr;
    }
    #endif
    
    
    uint8_t* curFB;
    #ifdef TRIPLEFB
    curFB = ( (uint8_t*) fbFull ) + ( writePtr * 96 * 64 );
    #else
    curFB = ( (uint8_t*) fbFull );
    #endif
    
    // Start from scratch.
    memset( curFB, 0, DWIDTH * DHEIGHT );
    
    // Draw players.
    drawRectangle( PADDLE1X, p1y, PADDLEW, PADDLEH );
    drawRectangle( PADDLE2X, p2y, PADDLEW, PADDLEH );
    drawRectangle( bx, by, BALLW, BALLW );
    
    // Draw middle line. If available with a lighter shade.
    drawVertLine( DWIDTH / 2, 0, DHEIGHT, 1 );
    
    #ifndef TRIPLEFB
    // Wait for VBLANK.
    while ( !frameDone ) {
      tight_loop_contents();
    }
    
    #else
    
    while ( ( time_us_64() - frameStart ) < frameTimeUS ) {
      tight_loop_contents();
    }    
    #endif
    
    frameDone = 0;
    
  }
}

void __not_in_flash_func( handleBuffer )(){
  // Frame buffer setup.
  uint8_t* fb = rom + FBOFFSET;
  
  while ( 1 ) {
    
    #ifdef TRIPLEFB
    // Advance read pointer?
    if ( frameBlendCnt == 0 ) {
      uint32_t nextReadPtr = readPtr + 1;
      if ( nextReadPtr == 3 ) {
        nextReadPtr = 0;
      }
      
      if ( nextReadPtr != writePtr ) {
        readPtr = nextReadPtr;
      }
    }
    #endif
    
    uint8_t* curFB;
    #ifdef TRIPLEFB
    curFB = ( (uint8_t*) fbFull ) + ( readPtr * 96 * 64 );
    #else
    curFB = ( (uint8_t*) fbFull );
    #endif
    
    // Transform the FB.
    for ( unsigned int y = 0; y < 8; ++y ) {
      for ( unsigned int x = 0; x < 96; ++x ) {
        unsigned tmp = 0;
        for ( unsigned int i = 0; i < 8; ++i ) {
          
          unsigned px = curFB[ x + y * 96 * 8 + i * 96 ];
          
          if ( px > frameBlendCnt ) {
            px = 1;
          } else {
            px = 0;
          }
          
          tmp |= px << i;
        }
        
        fb[ x + y * 96 ] = tmp;
      }
    }
    
    if ( frameBlendCnt == FRAMEBLEND ) {
      frameBlendCnt = 0;
      frameDone = 1;
    
      
    } else {
      ++frameBlendCnt;
    }
    
    uint32_t addrData;
    while ( 1 ) {
      if ( !pio_sm_is_rx_fifo_empty( pioWE_g, sm_we_g ) ) {
        // Got a write. Right lower address?
        pmKeys = pio_sm_get( pioWE_g, sm_we_g );
        addrData = pio_sm_get( pioWE_g, sm_we_addr_g );
        
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
  
  // Store the PIO variables related for write-handling global.
  pioWE_g = pioWE;
  sm_we_g = sm_we;
  sm_we_addr_g = sm_we_addr;

  // Handle frame buffer copying.
  multicore_launch_core1( handleBuffer );
  
  // Start actual application.
  doPongify( pioWE, sm_we, sm_we_addr );
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
