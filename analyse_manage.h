#ifndef _ANALYSE_MANAGE_H_
#define _ANALYSE_MANAGE_H_

#include <stdio.h>
#include "analyse.h"

/* 具体处理manage帧的部分 */

extern void deal_manage_mac( struct frame_info **fi_ptr, 
										const u_char *bytes );
extern void deal_manage_body( struct frame_info **fi_ptr, 
										const u_char *bytes );
extern void deal_fix_param( struct frame_info **fi_ptr, 
										const u_char *bytes );
extern void deal_cap_info( struct frame_info **fi_ptr, 
										const u_char *bytes );
extern void deal_ssid_param( struct frame_info **fi_ptr, 
										const u_char *bytes );
extern void deal_support_rates( struct frame_info **fi_ptr, 
										const u_char *bytes );
extern void deal_ds_param( struct frame_info **fi_ptr, 
										const u_char *bytes );
extern void deal_tim( struct frame_info **fi_ptr, const u_char *bytes );
extern void deal_erp( struct frame_info **fi_ptr, const u_char *bytes );
extern void deal_ext_support_rates( struct frame_info **fi_ptr,
										const u_char *bytes );

#endif
