// Multiple defines to select which firmware mode.
// MULTICART includes the multi ROM menu as the first loaded ROM
// Maximum ROM size for the MULTICART FW is currently 512 kB
//#define MULTICART

// EEPROM_RESTORE includes the EEPROM save/restore program as the 
// to be launched ROM file and handles EEPROM saves to the RP2040 Flash
// and loading from there.
//#define EEPROM_RESTORE

// When defining EEPROM_RESTORE and MULTICART, the EEPROM_RESTORE-specific
// functions are included and started, when the EEPROM_RESTORE program
// launched from the multi ROM menu is recognized.
// This makes only sense for PM2040 with larger memory like 16 MB,
// as otherwise the complete Flash memory (including the backups) will
// be overwritten when the multi ROM file will flashed onto.

#include "pico/stdlib.h"

#if defined(EEPROM_RESTORE) && !defined(MULTICART)
#include "eeprom_manager.h"

#elif !defined(MULTICART)
#include "rom.h"

#else
#define ROMSLOTS 2
// 20 ROM slots only for the larger variant.
//#define ROMSLOTS 20
#include "multirom.h"
#include "multimenu.h"
// For 16 MB Flash IC
//#include "multimenu_20slots.h"
#endif

#include "hardware/clocks.h"

#include "hardware/vreg.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/sync.h"

#include "oe.pio.h"
#include "pushData.pio.h"
#include "hale.pio.h"

#if defined(MULTICART) || defined(EEPROM_RESTORE)
#include "lale_32k.pio.h"
#include "writecheck.pio.h"
#include "writecheck_addr.pio.h"

#define DELAY 100000

#else
#include "lale.pio.h"
#endif

#if defined EEPROM_RESTORE
#include "hardware/flash.h"
#endif

#if defined MULTICART
#include "lale_512k.pio.h"
#define ROMSIZE 524288
#endif

#if defined(MULTICART) && defined(EEPROM_RESTORE)
#include <string.h>
#endif

// EEPROM_RESTORE defines.
#ifdef EEPROM_RESTORE
// Number of EEPROM backup slots
#define EEPROM_SLOTS 3
// Size of a single EEPROM backup
#define EEPROM_SIZE 8192
// Address of the first EEPROM backup
#define FLASHADDR ( PICO_FLASH_SIZE_BYTES - ( EEPROM_SIZE * EEPROM_SLOTS ) )
// Offset of the currently loaded EEPROM backup on the RP2040
#define EEPROM_OFFSET 0x4000
// Size of the EEPROM manager program.
#define EEPROM_MANAGER_SIZE 32768
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

#ifdef EEPROM_RESTORE
void __not_in_flash_func( loadEEPROM )( uint32_t slot, uint8_t* buffer ) {
  // Offset addr after the RAM (XIP_BASE).
  uint8_t* addr = (uint8_t*) ( XIP_BASE + FLASHADDR + ( slot * EEPROM_SIZE ) );
  
  // Go over byte-wise.
  // TODO: This is stupid, just read out 32b chunks.
  for ( int i = 0; i < EEPROM_SIZE; ++i ) {
    buffer[ i ] = *addr;
    ++addr;
  }
}

// Write the EEPROM buffer to Flash.
void __not_in_flash_func( writeEEPROM )( uint32_t slot, uint8_t* buffer ) {
  // Save RAM to Flash.
  // Not interrupt-safe.
  uint32_t ints = save_and_disable_interrupts();
  
  uint32_t curEEPROMAddr = FLASHADDR + ( slot * EEPROM_SIZE );

  // Bytes to be erased have to be a multiple of the sector size.
  // EEPROM is 8192 bytes large.
  // A flash sector is 4096 bytes.
  // So it's naturally a multiple.
  flash_range_erase( curEEPROMAddr, EEPROM_SIZE );

  // And write.
  // Bytes to be erased have to be a multiple of the page size.
  // EEPROM is 8192 bytes large.
  // A flash page size is 256 bytes.
  // So it's naturally a multiple.
  flash_range_program( curEEPROMAddr, buffer, EEPROM_SIZE );
  
  // Restore interrupts.
  restore_interrupts ( ints );
}
#endif

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
  
  #if defined(MULTICART) || defined(EEPROM_RESTORE)
  uint offset_lale = pio_add_program( pio, &lale_latch_32k_program );
  #else
  uint offset_lale = pio_add_program( pio, &lale_latch_program );
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
  
  #if defined(MULTICART) || defined(EEPROM_RESTORE)
  lale_latch_32k_program_init( pio, sm_lale, offset_lale, A0A10, LALE );
  #else
  lale_latch_program_init( pio, sm_lale, offset_lale, A0A10, LALE );
  #endif
  
  // Push the base address of the array.
  #if defined MULTICART
  pio_sm_put( pio, sm_lale, ( ( (uint32_t) rom_menu ) ) >> 15 );
  
  #elif defined EEPROM_RESTORE
  pio_sm_put( pio, sm_lale, ( ( (uint32_t) rom ) ) >> 15 ); // Adjust LALE address stuf.
  
  #else
  pio_sm_put( pio, sm_lale, ( ( (uint32_t) rom + XIP_NOCACHE_OFFSET ) ) >> 20 );
  
  #endif
  
  // Start the DMA channels.
  dma_start_channel_mask( 1u << hale_dma );
  dma_start_channel_mask( 1u << lale_addr_dma );

  #if defined(MULTICART) || defined(EEPROM_RESTORE)
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
  
  #endif
  
  // MULTICART-specific handling.
  uint32_t writeData;
  uint32_t addrData;
  #ifdef MULTICART
  // Wait till proper write.
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
  
  // EEPROM manager program?
  uint32_t eepromManager = 0;
  
  #ifdef EEPROM_RESTORE
  uint32_t gameCodeOffset = ( ROMSIZE * writeData ) + 0x21AC;
  if ( rom[ gameCodeOffset ] == 'E' &&
       rom[ gameCodeOffset + 1 ] == 'E' &&
       rom[ gameCodeOffset + 2 ] == 'P' &&
       rom[ gameCodeOffset + 3 ] == 'M' ) {
    eepromManager = 1;
  }
  #endif
  
  
  // Stop the LALE SM.
  pio_sm_set_enabled( pio, sm_lale, false );
  
  if ( !eepromManager ) {
    // Remove the old program.
    pio_remove_program( pio, &lale_latch_32k_program, offset_lale );
    
    // Add the new program at the same offset.
    pio_add_program_at_offset( pio, &lale_latch_program, offset_lale );
    
    // Restart the LALE SM.
    lale_latch_program_init( pio, sm_lale, offset_lale, A0A10, LALE );
    
    // Add the new ROM address.
    pio_sm_put( pio, sm_lale, ( ( romAddress + XIP_NOCACHE_OFFSET ) ) >> 19 );
      
    // Stop WE checking SMs.
    pio_sm_set_enabled( pioWE, sm_we, false );
    pio_sm_set_enabled( pioWE, sm_we_addr, false );
    
  } else {
    // Load the EEPROM manger into the memory which was used for the
    // multi ROM menu.
    memcpy( rom_menu, (uint8_t*) romAddress, EEPROM_MANAGER_SIZE );
    
    // Restart the LALE SM. Re-use the old SM.
    lale_latch_32k_program_init( pio, sm_lale, offset_lale, A0A10, LALE );
    
    // Add the new ROM address.
    pio_sm_put( pio, sm_lale, ( ( (uint32_t) rom_menu ) ) >> 15 );
    
    // Leave the write checking SMs on.
  }
  #endif
  
  #ifdef EEPROM_RESTORE
  // Load EEPROM data.
  uint32_t curEEPROMSlot = 0;
  uint32_t eepromPtr = 0;
  uint8_t* eeprom;
  
  #ifdef MULTICART
  eeprom = rom_menu + EEPROM_OFFSET;
  #else
  eeprom = rom + EEPROM_OFFSET;
  #endif

  loadEEPROM( curEEPROMSlot, eeprom );
  
  // Wait for writes.
  while ( 1 ) {
    if ( !pio_sm_is_rx_fifo_empty( pioWE, sm_we ) ) {
      // Got a write.
      writeData = pio_sm_get( pioWE, sm_we );
      addrData = pio_sm_get( pioWE, sm_we_addr );
      
      switch ( addrData ) {
        
        // Set the EEPROM slot.
        case 0b1111000001:
          curEEPROMSlot = writeData;
          // Reload the EEPROM.
          loadEEPROM( curEEPROMSlot, eeprom );
          break;
          
        // Set the lower EEPROM addr bytes.
        case 0b1111000010:
          eepromPtr = ( eepromPtr & 0xFFFFFF00 ) | writeData;
          break;
          
        // Set the higher EEPROM addr bytes.
        case 0b1111000011:
          eepromPtr = ( eepromPtr & 0xFFFF00FF ) | ( writeData << 8 );
          break;
          
        // Write to EEPROM buffer.
        case 0b1111000100:
          eeprom[ eepromPtr ] = writeData;
          break;
          
        // Transfer EEPROM buffer to Flash.
        case 0b1111000111:
          writeEEPROM( curEEPROMSlot, eeprom );
          break;
          
        // Unhandled case.
        default:
          break;
      }
      
    }
  }
  
  #endif

  
  // Do nothing.
  while ( 1 ) {
    tight_loop_contents();
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
