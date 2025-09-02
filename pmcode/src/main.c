#include "pm.h"

#include <stdint.h>
#include <string.h>

#define RPFB 0x4000
#define RPRUMBLE 0x4300
#define FB 0x1000
#define TXKEYS 0x5000

#define RUMBLEON  IO_DATA = ( IO_DATA | 0x10 )
#define RUMBLEOFF IO_DATA = ( IO_DATA & 0xEF )

void doGfx( void ) {

  // Set page 0
  LCD_CTRL = 0xB0;
  // Set hicol 0
  LCD_CTRL = 0x10;
  // Set hicol 0
  LCD_CTRL = 0x00;
  
  // Copy.
  #pragma asm
  ld a, #000h
  ld b, #040h
  ld ix, ba
  ld a, #0ffh
  ld b, #020h
  ld iy, ba
  #pragma endasm
  
  // loop.
  #pragma asm
  ld a, #00h
looppage0:
  ld [iy], [ix]
  inc ix
  inc a
  cp a, #060h
  jrs nz, looppage0
  #pragma endasm

  // Set page 1
  LCD_CTRL = 0xB1;
  // Set hicol 0
  LCD_CTRL = 0x10;
  // Set hicol 0
  LCD_CTRL = 0x00;
  
  // loop.
  #pragma asm
  ld a, #00h
looppage1:
  ld [iy], [ix]
  inc ix
  inc a
  cp a, #060h
  jrs nz, looppage1
  #pragma endasm

  // Set page 2
  LCD_CTRL = 0xB2;
  // Set hicol 0
  LCD_CTRL = 0x10;
  // Set hicol 0
  LCD_CTRL = 0x00;
  
  // loop.
  #pragma asm
  ld a, #00h
looppage2:
  ld [iy], [ix]
  inc ix
  inc a
  cp a, #060h
  jrs nz, looppage2
  #pragma endasm
  
  // Set page 3
  LCD_CTRL = 0xB3;
  // Set hicol 0
  LCD_CTRL = 0x10;
  // Set hicol 0
  LCD_CTRL = 0x00;
  
  // loop.
  #pragma asm
  ld a, #00h
looppage3:
  ld [iy], [ix]
  inc ix
  inc a
  cp a, #060h
  jrs nz, looppage3
  #pragma endasm
  
  // Set page 4
  LCD_CTRL = 0xB4;
  // Set hicol 0
  LCD_CTRL = 0x10;
  // Set hicol 0
  LCD_CTRL = 0x00;
  
  // loop.
  #pragma asm
  ld a, #00h
looppage4:
  ld [iy], [ix]
  inc ix
  inc a
  cp a, #060h
  jrs nz, looppage4
  #pragma endasm
  
  // Set page 5
  LCD_CTRL = 0xB5;
  // Set hicol 0
  LCD_CTRL = 0x10;
  // Set hicol 0
  LCD_CTRL = 0x00;
  
  // loop.
  #pragma asm
  ld a, #00h
looppage5:
  ld [iy], [ix]
  inc ix
  inc a
  cp a, #060h
  jrs nz, looppage5
  #pragma endasm
  
  // Set page 6
  LCD_CTRL = 0xB6;
  // Set hicol 0
  LCD_CTRL = 0x10;
  // Set hicol 0
  LCD_CTRL = 0x00;
  
  // loop.
  #pragma asm
  ld a, #00h
looppage6:
  ld [iy], [ix]
  inc ix
  inc a
  cp a, #060h
  jrs nz, looppage6
  #pragma endasm
  
  // Set page 7
  LCD_CTRL = 0xB7;
  // Set hicol 0
  LCD_CTRL = 0x10;
  // Set hicol 0
  LCD_CTRL = 0x00;
  
  // loop.
  #pragma asm
  ld a, #00h
looppage7:
  ld [iy], [ix]
  inc ix
  inc a
  cp a, #060h
  jrs nz, looppage7
  #pragma endasm
}

int main(void)
{
  uint8_t keys;
  
  // Key interrupts priority
  PRI_KEY(0x03);

  // Enable interrupts for keys (only power)
  IRQ_ENA3 = IRQ3_KEYPOWER;

  PRC_MODE = 0;
  
  // Set rumble direction.
  IO_DIR = IO_DIR | 0x10;
  
  // Don't do anything beside copying frame buffer.
  while ( 1 ) {
      
    // Wait for VBLANK.
    #pragma asm
    ld a, #08ah
    ld b, #020h
    ld ix, ba
waitsyncloop: 
    ld a, [ix]
    cp a, #010h
    jrs nz, waitsyncloop

waitsyncloop1:
    ld a, [ix]
    cp a, #010h
    jrs z, waitsyncloop1
    #pragma endasm

    doGfx();
    
    // Send keys.
    keys = ~KEY_PAD;
    *( (uint8_t *) TXKEYS ) = keys;
    
    // Get rumble.
    if ( *( (uint8_t *) RPRUMBLE ) ) {
      RUMBLEON;
    } else {
      RUMBLEOFF;
    }
  }
}
