#include "pm.h"

#include <stdint.h>
#include <string.h>

#define RPFB 0x4000
#define RPRUMBLE 0x4300
#define RPAUDIO  0x4301
#define RPAUDIO_VOL  0x4302
#define FB 0x1000
#define TXKEYS 0x5000


// Nr of frames to turn off.
#define PWRCNTOFF 150

#define RUMBLEON  IO_DATA = ( IO_DATA | 0x10 )
#define RUMBLEOFF IO_DATA = ( IO_DATA & 0xEF )


uint8_t pwrCnt = 0;

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
  
  // Audio stuff.
  
  // Timer 3 preset.
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
  
  // Set once the LCD low nibble.
  LCD_CTRL = 0;
  
  // Don't do anything beside copying frame buffer and audio.
  while ( 1 ) {
      
    #pragma asm
CPY MACRO 
  ld [BR:0FFh], [HL]
  inc HL
  ENDM
  
; Copy pixels and get new audio sample (address stored in IX)
CPY_12 MACRO
  ld [BR:04Ch], [IX]
  CPY
  CPY
  CPY
  CPY
  ld [BR:04Ch], [IX]
  CPY
  CPY
  CPY
  CPY
  ld [BR:04Ch], [IX]
  CPY
  CPY
  CPY
  CPY
  ENDM

CPY_ROW MACRO
  CPY_12
  CPY_12
  CPY_12
  CPY_12
  CPY_12
  CPY_12
  CPY_12
  CPY_12
  ENDM
  
  push all
  
  ; zero all additional registers.
  ld xp, #00h
  ld yp, #00h
  ld ep, #00h
  
  ; BR to 20.
  ld br, #020h
  
  ; load audio sample address.
  ld h, #043h
  ld l, #001h
  ld ix, hl
  
waitsyncloop:
; check vsync + load audio sample.
  ld [BR:04Ch], [IX]
  cp [BR:08Ah], #50
  jrs nz, waitsyncloop
  
  ; load RP FB address.
  ld h, #040h
  ld l, #000h
  
cpStart:  
  ; Set high nibble
  ld [BR:0FEh], #010h
  
  ; Set page 0.
  ld a, #0b0h
  ld [BR:0FEh], a
  
doCopy:
  CPY_ROW
  
  ; check page.
  cp a, #0b7h
  jrs z, doneCopy
  
  ; increase page.
  inc a
  
  ; set page and high nibble.
  ld [BR:0FEh], a
  ld [BR:0FEh], #010h
  jrl doCopy
  
doneCopy:
  ; Update the audio volume.
  ld h, #043h
  ld l, #002h
  ld [BR:071h], [HL]
  
  pop all
    #pragma endasm
    
    // Send keys.
    keys = ~KEY_PAD;
    *( (uint8_t *) TXKEYS ) = keys;
    
    // Power button?
    if ( keys & KEY_POWER ) {
      ++pwrCnt;
      
      if ( pwrCnt >= PWRCNTOFF ) {
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
