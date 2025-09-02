$CASE ON
  ;; Macro to add jump table elements
JP_FAR  MACRO lbl
  LD NB,#@CPAG(lbl)
  JRL lbl
  ENDM
  
  ;; Begin pokemon mini header
  DEFSECT ".min_header", CODE AT 2100H
  SECT    ".min_header"
  ASCII   "MN"
  JP_FAR  __START
  JP_FAR  _prc_frame_copy_irq
  JP_FAR  _prc_render_irq
  JP_FAR  _timer_2h_underflow_irq
  JP_FAR  _timer_2l_underflow_irq
  JP_FAR  _timer_1h_underflow_irq
  JP_FAR  _timer_1l_underflow_irq
  JP_FAR  _timer_3h_underflow_irq
  JP_FAR  _timer_3_cmp_irq
  JP_FAR  _timer_32hz_irq
  JP_FAR  _timer_8hz_irq
  JP_FAR  _timer_2hz_irq
  JP_FAR  _timer_1hz_irq
  JP_FAR  _ir_rx_irq
  JP_FAR  _shake_irq
  JP_FAR  _key_power_irq
  JP_FAR  _key_right_irq
  JP_FAR  _key_left_irq
  JP_FAR  _key_down_irq
  JP_FAR  _key_up_irq
  JP_FAR  _key_c_irq
  JP_FAR  _key_b_irq
  JP_FAR  _key_a_irq
  JP_FAR  _unknown_irq
  JP_FAR  _unknown_irq
  JP_FAR  _unknown_irq
  JP_FAR  _cartridge_irq
  ASCII   "NINTENDO"

  DEFSECT ".min_header_tail", CODE AT 21BCH
  SECT    ".min_header_tail"
  ASCII   "2P"
  DB      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

        ;; Begin startup code
	DEFSECT	".startup", CODE, SHORT
	SECT	".startup"
__start_cpt:
__START:
  ;==========================================================================
  ;===================  system initialization  ==============================
  ;==========================================================================
  LD      SP,#02000h                      ; stack pointer initialize
  LD      BR,#020h                        ; BR register initialize to I/O area
  LD      [BR:21h],#0Ch
  LD      [BR:25h],#080h
  LD      [BR:80h],#08h
  LD      [BR:81h],#08h
  ;LD     [BR:81h],#09h

  LD      SC, #0h

  LD      [BR:27h],#0FFh
  LD      [BR:28h],#0FFh
  LD      [BR:29h],#0FFh
  LD      [BR:2Ah],#0FFh
	CARL	__copytable
	CARL	_main
	CARL	__exit
	RETE


  GLOBAL  __start_cpt
  GLOBAL  __START
	EXTERN	(CODE) __copytable
	EXTERN	(CODE) _main
        EXTERN  (CODE) __exit
	CALLS	'_start_cpt', '_copytable'
	CALLS	'_start_cpt', 'main'
	CALLS	'_start_cpt', '_exit'

  EXTERN  _prc_frame_copy_irq
  EXTERN  _prc_render_irq
  EXTERN  _timer_2h_underflow_irq
  EXTERN  _timer_2l_underflow_irq
  EXTERN  _timer_1h_underflow_irq
  EXTERN  _timer_1l_underflow_irq
  EXTERN  _timer_3h_underflow_irq
  EXTERN  _timer_3_cmp_irq
  EXTERN  _timer_32hz_irq
  EXTERN  _timer_8hz_irq
  EXTERN  _timer_2hz_irq
  EXTERN  _timer_1hz_irq
  EXTERN  _ir_rx_irq
  EXTERN  _shake_irq
  EXTERN  _key_power_irq
  EXTERN  _key_right_irq
  EXTERN  _key_left_irq
  EXTERN  _key_down_irq
  EXTERN  _key_up_irq
  EXTERN  _key_c_irq
  EXTERN  _key_b_irq
  EXTERN  _key_a_irq
  EXTERN  _unknown_irq
  EXTERN  _cartridge_irq
        
  END
