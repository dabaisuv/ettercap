/*
    ettercap -- dissector TELNET -- TCP 23

    Copyright (C) ALoR & NaGA

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

    $Id: ec_telnet.c,v 1.1 2003/07/10 12:49:55 alor Exp $
*/

#include <ec.h>
#include <ec_decode.h>
#include <ec_dissect.h>
#include <ec_session.h>
#include <ec_strings.h>

/* protos */

FUNC_DECODER(dissector_telnet);
void telnet_init(void);
void skip_telnet_command(u_char **ptr, u_char *end);
int match_login_regex(char *ptr);

/************************************************/

/*
 * this function is the initializer.
 * it adds the entry in the table of registered decoder
 */

void __init telnet_init(void)
{
   dissect_add("telnet", APP_LAYER_TCP, 23, dissector_telnet);
}

/*
 * telnet sends characters one per packet,
 * so we have to make sessions to collect 
 * the string among the packet stram.
 *
 * once the user and pass are colected, the dissector
 * sets a fake session (containing 0xff) to remember
 * to not collect other chars.
 * if the login is not successful, the server will send
 * something like "*ogin". in this case, reset the fake
 * session and restart to collect characters
 */

FUNC_DECODER(dissector_telnet)
{
   DECLARE_DISP_PTR_END(ptr, end);
   struct session *s = NULL;
   void *ident = NULL;
   char tmp[MAX_ASCII_ADDR_LEN];

   /* the connection is starting... create the session */
   CREATE_SESSION_ON_SYN_ACK("telnet", s);
   
   /* skip empty packets (ACK packets) */
   if (PACKET->DATA.len == 0)
      return NULL;
   
   DEBUG_MSG("TELNET --> TCP dissector_telnet");

   /* move the pointer to skip commands */
   skip_telnet_command(&ptr, end);

   /* the packet was made only by commands, skip it */
   if (ptr == end)
      return NULL;

   /* is the message from the server or the client ? */
   if (dissect_on_port("telnet", ntohs(PACKET->L4.src)) == ESUCCESS) {
      
      /* the login was not successful, restart the collecting */
      if (match_login_regex(ptr)) {
         dissect_create_ident(&ident, PACKET);
         session_del(ident);
      }
   } else {
      
      /* create an ident to retrieve the session */
      dissect_create_ident(&ident, PACKET);
      /* retrieve the session */
      if (session_get(&s, ident) == -ENOTFOUND) {
         /* create the new session and save the first char */
         dissect_create_session(&s, PACKET);
         /* remember the state (used later) */
         s->data = strdup(ptr);
         /* save the session */
         session_put(s);
      } else {
         char str[strlen(s->data) + 2];

         memset(str, 0, sizeof(str));
        
         /* this is the fake session, user and pass already collected */
         if (*(char *)s->data == '\xff')
            return NULL;

         /* concat the char to the previous one */
         sprintf(str, "%s%c", (char *)s->data, *ptr);

         /* save the new string */
         SAFE_FREE(s->data);
         s->data = strdup(str);
         
         /* 
          * the user input is terminated
          * check if it was the password by checking
          * the presence of \r in the string
          * we store "user\rpass\r" and then we split it
          */
         if (strchr(ptr, '\r') || strchr(ptr, '\n')) {
            /* there is the \r and it is not the last char */
            if ( ((ptr = strchr(s->data, '\r')) || (ptr = strchr(s->data, '\n')))
                  && ptr != s->data + strlen(s->data) - 1 ) {

               /* fill the structure */
               PACKET->DISSECTOR.user = strdup(s->data);
               if ( (ptr = strchr(PACKET->DISSECTOR.user, '\r')) != NULL )
                  *ptr = '\0';
      
               PACKET->DISSECTOR.pass = strdup(ptr + 1);
               if ( (ptr = strchr(PACKET->DISSECTOR.pass, '\r')) != NULL )
                  *ptr = '\0';
               
               /* 
                * set a fake session to remember we have 
                * collected the user and pass 
                */
               SAFE_FREE(s->data);
               s->data = strdup("\xff");
               
               /* display the message */
               USER_MSG("TELNET : %s:%d -> USER: %s  PASS: %s\n", ip_addr_ntoa(&PACKET->L3.dst, tmp),
                                    ntohs(PACKET->L4.dst), 
                                    PACKET->DISSECTOR.user,
                                    PACKET->DISSECTOR.pass);
            }
            return NULL;
         }
      }
     
   }
   

   /* check if it is the first readable packet sent by the server */
   IF_FIRST_PACKET_FROM_SERVER("telnet", s, ident) {
      
      size_t i;
            
      DEBUG_MSG("\tdissector_telnet BANNER");
      /* get the banner */
      PACKET->DISSECTOR.banner = strdup(ptr);
      ptr = PACKET->DISSECTOR.banner;
      /* replace \r\n with spaces */ 
      for (i = 0; i < strlen(ptr); i++) {
         if (ptr[i] == '\r' || ptr[i] == '\n')
            ptr[i] = ' ';
      }
     
   } ENDIF_FIRST_PACKET_FROM_SERVER(s, ident)
   

   return NULL;
}

/*
 * move the pointer ptr while it is a telnet command.
 */
void skip_telnet_command(u_char **ptr, u_char *end)
{
   while(**ptr == 0xff && *ptr != end) {
      /* sub option 0xff 0xfa ... ... 0xff 0xf0 */
      if (*(*ptr + 1) == 0xfa) {
         *ptr += 1;
         /* search the sub-option end (0xff 0xf0) */
         do {
            *ptr += 1;
         } while(**ptr != 0xff && *ptr != end);
         /* skip the sub-option end */
         *ptr += 2;
      } else {
      /* normal option 0xff 0xXX 0xXX */
         *ptr += 3;
      }
   }
}

/* 
 * serach the strings which can identify failed login...
 * return 1 on succes, 0 on failure
 */
int match_login_regex(char *ptr)
{
   regex_t *regex;
   int ret = 0;

   /*
    * matches: 
    *    - login at the beginning of the buffer
    *    - inccorect
    *    - failed
    *    - failure
    */
#define LOGIN_REGEX "\\`login.*|.*incorrect.*|.*failed.*|.*failure.*"
   
   /* allocate the new structure */
   regex = calloc(1, sizeof(regex_t));
   ON_ERROR(regex, NULL, "can't allocate memory");

   /* failed compilation of regex */
   if (regcomp(regex, LOGIN_REGEX, REG_EXTENDED | REG_NOSUB | REG_ICASE ) != 0) {
      SAFE_FREE(regex);
      return 0;
   }

   /* execute the regex */
   if (regexec(regex, ptr, 0, NULL, 0) == 0)
      ret = 1;
    
   SAFE_FREE(regex);
   return ret;
}

/* EOF */

// vim:ts=3:expandtab

