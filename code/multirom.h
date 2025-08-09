// For a 16MB Flash, use the larger declaration.
const uint8_t rom[ 2 * 524288 ] __attribute__ ((section(".romStorage"))) = {
//const uint8_t rom[ 20 * 524288 ] __attribute__ ((section(".romStorage"))) = {
  'R', 'O', 'M', 'S', 'T', 'A', 'R', 'T' };
