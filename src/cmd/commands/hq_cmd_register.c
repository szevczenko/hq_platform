#include "hq_cmd.h"

/* -- Command registration functions (one per command file) ---------- */
extern void hq_cmd_hello_register( void );
extern void hq_cmd_wifi_register( void );

void hq_cmd_register_builtin_commands( void )
{
    hq_cmd_hello_register();
    hq_cmd_wifi_register();
}
