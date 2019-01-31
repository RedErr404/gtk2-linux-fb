/* GTK - The GIMP Toolkit
 * gtkcupsutils.h: Statemachine implementation of POST and GET 
 * cup calls which can be used to create a non-blocking cups API
 * Copyright (C) 2003, Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gtkcupsutils.h"
#include "config.h"
#include "gtkdebug.h"

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <time.h>

#if CUPS_VERSION_MAJOR > 1 || (CUPS_VERSION_MAJOR == 1 && CUPS_VERSION_MINOR > 1) || (CUPS_VERSION_MAJOR == 1 && CUPS_VERSION_MINOR == 1 && CUPS_VERSION_PATCH >= 20)
#define HAVE_HTTP_AUTHSTRING 1
#endif

typedef void (*GtkCupsRequestStateFunc) (GtkCupsRequest *request);

static void _connect            (GtkCupsRequest *request);
static void _post_send          (GtkCupsRequest *request);
static void _post_write_request (GtkCupsRequest *request);
static void _post_write_data    (GtkCupsRequest *request);
static void _post_check         (GtkCupsRequest *request);
static void _post_read_response (GtkCupsRequest *request);

static void _get_send           (GtkCupsRequest *request);
static void _get_check          (GtkCupsRequest *request);
static void _get_read_data      (GtkCupsRequest *request);

struct _GtkCupsResult
{
  gchar *error_msg;
  ipp_t *ipp_response;
  GtkCupsErrorType error_type;

  /* some error types like HTTP_ERROR have a status and a code */
  int error_status;            
  int error_code;

  guint is_error : 1;
  guint is_ipp_response : 1;
};


#define _GTK_CUPS_MAX_ATTEMPTS 10 
#define _GTK_CUPS_MAX_CHUNK_SIZE 8192

GtkCupsRequestStateFunc post_states[] = {_connect,
                                         _post_send,
                                         _post_write_request,
                                         _post_write_data,
                                         _post_check,
                                         _post_read_response};

GtkCupsRequestStateFunc get_states[] = {_connect,
                                        _get_send,
                                        _get_check,
                                        _get_read_data};

static void
gtk_cups_result_set_error (GtkCupsResult    *result,
                           GtkCupsErrorType  error_type,
                           int               error_status,
                           int               error_code, 
                           const char       *error_msg,
			   ...)
{
  va_list args;

  result->is_ipp_response = FALSE;
  result->is_error = TRUE;
  result->error_type = error_type;
  result->error_status = error_status;
  result->error_code = error_code;

  va_start (args, error_msg);
  result->error_msg = g_strdup_vprintf (error_msg, args);
  va_end (args);
}

GtkCupsRequest *
gtk_cups_request_new (http_t *connection,
                      GtkCupsRequestType req_type, 
                      gint operation_id,
                      GIOChannel *data_io,
                      const char *server,
                      const char *resource)
{
  GtkCupsRequest *request;
  cups_lang_t *language;
  
  request = g_new0 (GtkCupsRequest, 1);

  return request;
}

static void
gtk_cups_result_free (GtkCupsResult *result)
{
}

void
gtk_cups_request_free (GtkCupsRequest *request)
{
}

gboolean 
gtk_cups_request_read_write (GtkCupsRequest *request)
{
  if (request->type == GTK_CUPS_POST)
    post_states[request->state](request);
  else if (request->type == GTK_CUPS_GET)
    get_states[request->state](request);

  if (request->attempts > _GTK_CUPS_MAX_ATTEMPTS && 
      request->state != GTK_CUPS_REQUEST_DONE)
    {
      /* TODO: should add a status or error code for too many failed attempts */
      gtk_cups_result_set_error (request->result, 
                                 GTK_CUPS_ERROR_GENERAL,
                                 0,
                                 0, 
                                 "Too many failed attempts");

      request->state = GTK_CUPS_REQUEST_DONE;
      request->poll_state = GTK_CUPS_HTTP_IDLE;
    }
    
  if (request->state == GTK_CUPS_REQUEST_DONE)
    {
      request->poll_state = GTK_CUPS_HTTP_IDLE;
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

GtkCupsPollState 
gtk_cups_request_get_poll_state (GtkCupsRequest *request)
{
  return request->poll_state;
}



GtkCupsResult *
gtk_cups_request_get_result (GtkCupsRequest *request)
{
  return request->result;
}

void            
gtk_cups_request_ipp_add_string (GtkCupsRequest *request,
                                 ipp_tag_t group,
                                 ipp_tag_t tag,
                                 const char *name,
                                 const char *charset,
                                 const char *value)
{
}

void            
gtk_cups_request_ipp_add_strings (GtkCupsRequest *request,
				  ipp_tag_t group,
				  ipp_tag_t tag,
				  const char *name,
				  int num_values,
				  const char *charset,
				  const char * const *values)
{
}



typedef struct
{
  const char	*name;
  ipp_tag_t	value_tag;
} ipp_option_t;

static const ipp_option_t ipp_options[] =
			{
			  { "blackplot",		IPP_TAG_BOOLEAN },
			  { "brightness",		IPP_TAG_INTEGER },
			  { "columns",			IPP_TAG_INTEGER },
			  { "copies",			IPP_TAG_INTEGER },
			  { "finishings",		IPP_TAG_ENUM },
			  { "fitplot",			IPP_TAG_BOOLEAN },
			  { "gamma",			IPP_TAG_INTEGER },
			  { "hue",			IPP_TAG_INTEGER },
			  { "job-k-limit",		IPP_TAG_INTEGER },
			  { "job-page-limit",		IPP_TAG_INTEGER },
			  { "job-priority",		IPP_TAG_INTEGER },
			  { "job-quota-period",		IPP_TAG_INTEGER },
			  { "landscape",		IPP_TAG_BOOLEAN },
			  { "media",			IPP_TAG_KEYWORD },
			  { "mirror",			IPP_TAG_BOOLEAN },
			  { "natural-scaling",		IPP_TAG_INTEGER },
			  { "number-up",		IPP_TAG_INTEGER },
			  { "orientation-requested",	IPP_TAG_ENUM },
			  { "page-bottom",		IPP_TAG_INTEGER },
			  { "page-left",		IPP_TAG_INTEGER },
			  { "page-ranges",		IPP_TAG_RANGE },
			  { "page-right",		IPP_TAG_INTEGER },
			  { "page-top",			IPP_TAG_INTEGER },
			  { "penwidth",			IPP_TAG_INTEGER },
			  { "ppi",			IPP_TAG_INTEGER },
			  { "prettyprint",		IPP_TAG_BOOLEAN },
			  { "printer-resolution",	IPP_TAG_RESOLUTION },
			  { "print-quality",		IPP_TAG_ENUM },
			  { "saturation",		IPP_TAG_INTEGER },
			  { "scaling",			IPP_TAG_INTEGER },
			  { "sides",			IPP_TAG_KEYWORD },
			  { "wrap",			IPP_TAG_BOOLEAN }
			};


static ipp_tag_t
_find_option_tag (const gchar *option)
{
  int lower_bound, upper_bound, num_options;
  int current_option;
  ipp_tag_t result;

  result = IPP_TAG_ZERO;

  lower_bound = 0;
  upper_bound = num_options = (int)(sizeof(ipp_options) / sizeof(ipp_options[0])) - 1;
  
  while (1)
    {
      int match;
      current_option = (int) (((upper_bound - lower_bound) / 2) + lower_bound);

      match = strcasecmp(option, ipp_options[current_option].name);
      if (match == 0)
        {
	  result = ipp_options[current_option].value_tag;
	  return result;
	}
      else if (match < 0)
        {
          upper_bound = current_option - 1;
	}
      else
        {
          lower_bound = current_option + 1;
	}

      if (upper_bound == lower_bound && upper_bound == current_option)
        return result;

      if (upper_bound < 0)
        return result;

      if (lower_bound > num_options)
        return result;

      if (upper_bound < lower_bound)
        return result;
    }
}

/*
 * Note that this function uses IPP_TAG_JOB, so it is
 * only suitable for IPP Group 2 attributes.
 * See RFC 2911.
 */
void
gtk_cups_request_encode_option (GtkCupsRequest *request,
                                const gchar    *option,
			        const gchar    *value)
{
  ipp_tag_t option_tag;

  g_assert (option != NULL);
  g_assert (value != NULL);

  option_tag = _find_option_tag (option);

  if (option_tag == IPP_TAG_ZERO)
    {
      option_tag = IPP_TAG_NAME;
      if (strcasecmp (value, "true") == 0 ||
          strcasecmp (value, "false") == 0)
        {
          option_tag = IPP_TAG_BOOLEAN;
        }
    }
        
  switch (option_tag)
    {
      case IPP_TAG_INTEGER:
      case IPP_TAG_ENUM:
        ippAddInteger (request->ipp_request,
                       IPP_TAG_JOB,
                       option_tag,
                       option,
                       strtol (value, NULL, 0));
        break;

      case IPP_TAG_BOOLEAN:
        {
          char b;
          b = 0;
          if (!strcasecmp(value, "true") ||
	      !strcasecmp(value, "on") ||
	      !strcasecmp(value, "yes")) 
	    b = 1;
	  
          ippAddBoolean(request->ipp_request,
                        IPP_TAG_JOB,
                        option,
                        b);
        
          break;
        }
        
      case IPP_TAG_RANGE:
        {
          char	*s;
          int lower;
          int upper;
          
          if (*value == '-')
	    {
	      lower = 1;
	      s = (char *)value;
	    }
	  else
	    lower = strtol(value, &s, 0);

	  if (*s == '-')
	    {
	      if (s[1])
		upper = strtol(s + 1, NULL, 0);
	      else
		upper = 2147483647;
            }
	  else
	    upper = lower;
         
          ippAddRange (request->ipp_request,
                       IPP_TAG_JOB,
                       option,
                       lower,
                       upper);

          break;
        }

      case IPP_TAG_RESOLUTION:
        {
          char *s;
          int xres;
          int yres;
          ipp_res_t units;
          
          xres = strtol(value, &s, 0);

	  if (*s == 'x')
	    yres = strtol(s + 1, &s, 0);
	  else
	    yres = xres;

	  if (strcasecmp(s, "dpc") == 0)
            units = IPP_RES_PER_CM;
          else
            units = IPP_RES_PER_INCH;
          
          ippAddResolution (request->ipp_request,
                            IPP_TAG_JOB,
                            option,
                            units,
                            xres,
                            yres);

          break;
        }

      default:
        {
          char *values;
          char *s;
          int in_quotes;
          char *next;
          GPtrArray *strings;
          
          values = g_strdup (value);
          strings = NULL;
          in_quotes = 0;
          
          for (s = values, next = s; *s != '\0'; s++)
            {
              if (in_quotes != 2 && *s == '\'')
                {
                  /* skip quoted value */
                  if (in_quotes == 0)
                    in_quotes = 1;
                  else
                    in_quotes = 0;
                }
              else if (in_quotes != 1 && *s == '\"')
                {
                  /* skip quoted value */
                  if (in_quotes == 0)
                    in_quotes = 2;
                  else
                    in_quotes = 0;
                }
              else if (in_quotes == 0 && *s == ',')
                {
                  /* found delimiter, add to value array */
                  *s = '\0';
                  if (strings == NULL)
                    strings = g_ptr_array_new ();
                  g_ptr_array_add (strings, next);
                  next = s + 1;
                }
              else if (in_quotes == 0 && *s == '\\' && s[1] != '\0')
                {
                  /* skip escaped character */
                  s++;
                }
            }
          
          if (strings == NULL)
            {
              /* single value */
              ippAddString (request->ipp_request,
                            IPP_TAG_JOB,
                            option_tag,
                            option,
                            NULL,
                            value);
            }
          else
            {
              /* multiple values */
              
              /* add last value */
              g_ptr_array_add (strings, next);
              
              ippAddStrings (request->ipp_request,
                             IPP_TAG_JOB,
                             option_tag,
                             option,
                             strings->len,
                             NULL,
                             (const char **) strings->pdata);
              g_ptr_array_free (strings, TRUE);
            }

          g_free (values);
        }

        break;
    }
}
				

static void
_connect (GtkCupsRequest *request)
{
  request->poll_state = GTK_CUPS_HTTP_IDLE;

  if (request->http == NULL)
    {
      request->http = httpConnectEncrypt (request->server, ippPort(), cupsEncryption());

      if (request->http == NULL)
        request->attempts++;

      if (request->http)
        httpBlocking (request->http, 0);
        
      request->own_http = TRUE;
    }
  else
    {
      request->attempts = 0;
      request->state++;

      /* we always write to the socket after we get
         the connection */
      request->poll_state = GTK_CUPS_HTTP_WRITE;
    }
}

static void 
_post_send (GtkCupsRequest *request)
{
}

static void 
_post_write_request (GtkCupsRequest *request)
{
}

static void 
_post_write_data (GtkCupsRequest *request)
{
}

static void 
_post_check (GtkCupsRequest *request)
{
}

static void 
_post_read_response (GtkCupsRequest *request)
{
}

static void 
_get_send (GtkCupsRequest *request)
{
}

static void 
_get_check (GtkCupsRequest *request)
{

}

static void 
_get_read_data (GtkCupsRequest *request)
{
}

gboolean
gtk_cups_request_is_done (GtkCupsRequest *request)
{
  return (request->state == GTK_CUPS_REQUEST_DONE);
}

gboolean
gtk_cups_result_is_error (GtkCupsResult *result)
{
  return result->is_error;
}

ipp_t *
gtk_cups_result_get_response (GtkCupsResult *result)
{
  return result->ipp_response;
}

GtkCupsErrorType
gtk_cups_result_get_error_type (GtkCupsResult *result)
{
  return result->error_type;
}

int
gtk_cups_result_get_error_status (GtkCupsResult *result)
{
  return result->error_status;
}

int
gtk_cups_result_get_error_code (GtkCupsResult *result)
{
  return result->error_code;
}

const char *
gtk_cups_result_get_error_string (GtkCupsResult *result)
{
  return result->error_msg; 
}

