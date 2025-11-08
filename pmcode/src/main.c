#include "pm.h"

#include <stdint.h>
#include <string.h>

#define RPFB 0x4000
#define RPRUMBLE 0x4300
#define RPAUDIO  0x4301
#define RPAUDIO_VOL  0x4302
#define FB 0x1000
#define TXKEYS 0x5000

#define RUMBLEON  IO_DATA = ( IO_DATA | 0x10 )
#define RUMBLEOFF IO_DATA = ( IO_DATA & 0xEF )

// Nr of frames to turn off.
#define PWRCNTOFF 150

void doGfx( void ) {  
  // Copy.
  #pragma asm
  ; Set page 0 and hi + low nibble.
  ld ep, #00h
  ld b, #0b0h
  ld [020feh], b
  
  ld b, #010h
  ld [020feh], b
  
  ld b, #000h
  ld [020feh], b
  
  ; Load addresses.
  ld a, #000h
  ld b, #040h
  ld ix, ba
  
  ld a, #0ffh
  ld b, #020h
  ld iy, ba
  
  ; Get audio volume
  ld b, [04302h]
  ; Set it.
  ld [02071h], b
  
looppage0:
  ; Get audio.
  ld b, [04301h]
  ; Set pivot (low).
  ld [0204ch], b
  
  ; load FB data.
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  
  cp ix, #04060h
  jrs nz, looppage0

  ; Set page 1.
  ld b, #0b1h
  ld [020feh], b
  
  ld b, #010h
  ld [020feh], b
  
  ld b, #000h
  ld [020feh], b
  
looppage1:
  ; Get audio.
  ld b, [04301h]
  ; Set pivot (low).
  ld [0204ch], b
  
  ; load FB data.
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  
  cp ix, #040c0h
  jrs nz, looppage1

; Set page 2.
  ld b, #0b2h
  ld [020feh], b
  
  ld b, #010h
  ld [020feh], b
  
  ld b, #000h
  ld [020feh], b

looppage2:
  ; Get audio.
  ld b, [04301h]
  ; Set pivot (low).
  ld [0204ch], b
  
  ; load FB data.
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  
  cp ix, #04120h
  jrs nz, looppage2

  ; Set page 3.
  ld b, #0b3h
  ld [020feh], b
  
  ld b, #010h
  ld [020feh], b
  
  ld b, #000h
  ld [020feh], b

looppage3:
  ; Get audio.
  ld b, [04301h]
  ; Set pivot (low).
  ld [0204ch], b
  
  ; load FB data.
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  
  cp ix, #04180h
  jrs nz, looppage3
  
  ; Set page 4.
  ld b, #0b4h
  ld [020feh], b
  
  ld b, #010h
  ld [020feh], b
  
  ld b, #000h
  ld [020feh], b
  
looppage4:
  ; Get audio.
  ld b, [04301h]
  ; Set pivot (low).
  ld [0204ch], b
  
  ; load FB data.
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  
  cp ix, #041e0h
  jrs nz, looppage4
  
  ; Set page 5.
  ld b, #0b5h
  ld [020feh], b
  
  ld b, #010h
  ld [020feh], b
  
  ld b, #000h
  ld [020feh], b

looppage5:
  ; Get audio.
  ld b, [04301h]
  ; Set pivot (low).
  ld [0204ch], b
  
  ; load FB data.
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  
  cp ix, #04240h
  jrs nz, looppage5
  
  
  ; Set page 6.
  ld b, #0b6h
  ld [020feh], b
  
  ld b, #010h
  ld [020feh], b
  
  ld b, #000h
  ld [020feh], b

looppage6:
  ; Get audio.
  ld b, [04301h]
  ; Set pivot (low).
  ld [0204ch], b

  ; load FB data.
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  
  cp ix, #042a0h
  jrs nz, looppage6
  
  ; Set page 7.
  ld b, #0b7h
  ld [020feh], b
  
  ld b, #010h
  ld [020feh], b
  
  ld b, #000h
  ld [020feh], b
  
looppage7:
  ; Get audio.
  ld b, [04301h]
  ; Set pivot (low).
  ld [0204ch], b
  
  ; load FB data.
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  ld [iy], [ix]
  inc ix
  
  cp ix, #04300h
  jrs nz, looppage7
  
  #pragma endasm
}

int main(void)
{
  uint8_t keys;
  uint8_t pwrCnt = 0;
  
  // Key interrupts priority
  PRI_KEY(0x03);

  // Enable interrupts for keys (only power)
  IRQ_ENA3 = IRQ3_KEYPOWER;

  PRC_MODE = 0;
  
  // Set rumble direction.
  IO_DIR = IO_DIR | 0x10;
  
  // Audio stuff.
  
  // Timer 3 preset.
  // 4b audio.
  //TMR3_PRE = 0x000F;
  // 5b audio.
  TMR3_PRE = 0x0020;
  
  // Timer 3 pivot init.
  TMR3_PVT = TMR3_PRE;
  
  // Enable oscillator 1 (CPU clock)
  TMR1_OSC = _BV(5);
  
  // Prescale. Enable both timers. Set 0 prescale.
  TMR3_SCALE = _BV(7) |_BV(3);
  
  // Select oscillator 1.
  TMR3_OSC = 0x00;
  
  // Control. Set Enable and reset.
  TMR3_CTRL_L = _BV(7) | _BV( 1 ) | _BV( 2 );
  
  // Initial audio volume.
  AUD_VOL = 0x03;
  
  // Unmute.
  AUD_CTRL = 0;
  
  // Don't do anything beside copying frame buffer and audio.
  while ( 1 ) {
      
    // Wait for VBLANK.
    #pragma asm
    ld a, #08ah
    ld b, #020h
    ld ix, ba
waitsyncloop:
    ; Get audio.
    ld b, [04301h]
    ; Set pivot (low).
    ld [0204ch], b
  
    ld a, [ix]
    cp a, #010h
    jrs nz, waitsyncloop

waitsyncloop1:
    ; Get audio.
    ld b, [04301h]
    ; Set pivot (low).
    ld [0204ch], b

    ld a, [ix]
    cp a, #010h
    jrs z, waitsyncloop1
    #pragma endasm

    doGfx();
    
    // Send keys.
    keys = ~KEY_PAD;
    *( (uint8_t *) TXKEYS ) = keys;
    
    // Power button?
    if ( keys & KEY_POWER ) {
      ++pwrCnt;
      
      if ( pwrCnt >= PWRCNTOFF ) {
        // Turn off.
        _int(0x48);
      }
      
    } else {
      pwrCnt = 0;
    }
    
    // Get rumble.
    if ( *( (uint8_t *) RPRUMBLE ) ) {
      RUMBLEON;
    } else {
      RUMBLEOFF;
    }
  }
}
