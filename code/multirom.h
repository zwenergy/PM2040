// For a 16MB Flash, use the larger declaration.
#ifdef PM2040_SLOTS20
const uint8_t rom[ 20 * 524288 ] __attribute__ ((section(".romStorage"))) = {
#else
const uint8_t rom[ 2 * 524288 ] __attribute__ ((section(".romStorage"))) = {
//const uint8_t rom[ 20 * 524288 ] __attribute__ ((section(".romStorage"))) = {
#endif
  'R', 'O', 'M', 'S', 'T', 'A', 'R', 'T' };
