#include "pm.h"

const _rom char game_code[4] _at(0x21AC) = "FB";
const _rom char game_title[12] _at(0x21B0) = "FBCOPY";

void _interrupt(0) prc_frame_copy_irq(void) {
    _slp();
}
void _interrupt(0) prc_render_irq(void) {
    _slp();
}
void _interrupt(0) timer_2h_underflow_irq(void) {
  _slp();
}
void _interrupt(0) timer_2l_underflow_irq(void) {
  _slp();
}
void _interrupt(0) timer_1h_underflow_irq(void) {
  _slp();
}
void _interrupt(0) timer_1l_underflow_irq(void) {
  _slp();
}
void _interrupt(0) timer_3h_underflow_irq(void) {
  _slp();
}
void _interrupt(0) timer_3_cmp_irq(void) {
  _slp();
}
void _interrupt(0) timer_32hz_irq(void) {
  _slp();
}
void _interrupt(0) timer_8hz_irq(void) {
  _slp();
}
void _interrupt(0) timer_2hz_irq(void) {
  _slp();
}
void _interrupt(0) timer_1hz_irq(void) {
  _slp();
}
void _interrupt(0) ir_rx_irq(void) {
  _slp();
}
void _interrupt(0) shake_irq(void) {
  _slp();
}
void _interrupt(0) key_power_irq(void) {
  IRQ_ACT3 = IRQ3_KEYPOWER;
}
void _interrupt(0) key_right_irq(void) {
  _slp();
}
void _interrupt(0) key_left_irq(void) {
  _slp();
}
void _interrupt(0) key_down_irq(void) {
  _slp();
}
void _interrupt(0) key_up_irq(void) {
  _slp();
}
void _interrupt(0) key_c_irq(void) {
  _slp();
}
void _interrupt(0) key_b_irq(void) {
  _slp();
}
void _interrupt(0) key_a_irq(void) {
  _slp();
}
void _interrupt(0) unknown_irq(void) {
  _slp();
}
void _interrupt(0) cartridge_irq(void) {
  _slp();
}

void _exit( int i )
{
  _int(0x48);
}
