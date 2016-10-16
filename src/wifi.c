#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libopencmsis/core_cm3.h>

#include "wifi.h"

// wifi_rb is the ring buffer that receives all incoming data from the WIFI
// module, data is written to it during an interrupt on the USART and read from
// it in the wifi sys tick handler.
uint8_t wifi_rb_space[RING_BUFF_SIZE];
ring_buffer_t wifi_rb = {wifi_rb_space, 0, 0, 0};

// tmp_buffer is used to process WIND from the WIFI module.
uint8_t tmp_buffer[WIFI_TMP_BUFF_SIZE];

// wifi_at_state is the global state for the current AT command, state will be
// reset before executing a new AT command.
uint8_t at_buff_space[WIFI_AT_BUFF_SIZE];
wifi_at_t wifi_at_state = {AT_STATUS_CLEAR, &at_buff_space[0], 0, 0};

// wifi_state tracks the current state of the WIFI module.
volatile wifi_state_t wifi_state = WIFI_STATE_OFF;

// wifi_recv_state tracks the expected response type from the WIFI module, it is
// used to decide what kind of processing is done on the response.
static wifi_recv_t wifi_recv_state = recv_async_indication;

static void wifi_gpio_setup(void);
static void wifi_usart_setup(void);

static void wifi_debug_print_buff(uint8_t *const buff, uint8_t prefix);
bool wifi_process_async_response(uint8_t *const buff, uint8_t data);
bool wifi_process_wind(wifi_state_t *state, uint8_t *const buff_ptr);
bool wifi_process_bttn_indication(uint8_t *const buff);
bool wifi_process_at_response(wifi_at_t *at, uint8_t data);
uint16_t wifi_http_parse_status(uint8_t *response);

void wifi_init(void) {
  wifi_gpio_setup();
  wifi_usart_setup();
  wifi_on();
}

void wifi_on(void) {
  gpio_set(GPIOB, GPIO2);  // Power on Wifi module.
}

void wifi_off(void) {
  gpio_clear(GPIOB, GPIO2);  // Power off Wifi module.
}

// wifi_soft_reset executes the AT+CFUN command, issuing a reset, and waits for
// the power on indication from the WIFI module.
void wifi_soft_reset(void) {
  // We must wait for the console to be active.
  wifi_wait_state(WIFI_STATE_CONSOLE_ACTIVE);

  // Unset the power on state so that we can wait for it.
  wifi_state &= ~(WIFI_STATE_POWER_ON);

  wifi_send_string("AT+CFUN=1\r");  // Reset wifi module.
  wifi_wait_state(WIFI_STATE_POWER_ON);
}

void wifi_hard_reset(void) {
  wifi_off();
  wifi_state = WIFI_STATE_OFF;
  delay(1000);
  wifi_on();
  wifi_wait_state(WIFI_STATE_POWER_ON);
}

static void wifi_gpio_setup(void) {
  rcc_periph_clock_enable(RCC_WIFI_USART);

  gpio_mode_setup(WIFI_GPIO_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, WIFI_GPIO_TX);
  gpio_set_output_options(WIFI_GPIO_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ,
                          WIFI_GPIO_TX);

  gpio_mode_setup(WIFI_GPIO_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, WIFI_GPIO_RX);
  gpio_set_output_options(WIFI_GPIO_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ,
                          WIFI_GPIO_RX);

  gpio_set_af(WIFI_GPIO_PORT, GPIO_AF7, WIFI_GPIO_TX);
  gpio_set_af(WIFI_GPIO_PORT, GPIO_AF7, WIFI_GPIO_RX);
}

static void wifi_usart_setup(void) {
  usart_set_baudrate(WIFI_USART, 115200);
  usart_set_databits(WIFI_USART, 8);
  usart_set_stopbits(WIFI_USART, USART_STOPBITS_1);
  usart_set_mode(WIFI_USART, USART_MODE_TX_RX);
  usart_set_parity(WIFI_USART, USART_PARITY_NONE);
  usart_set_flow_control(WIFI_USART, USART_FLOWCONTROL_NONE);

  nvic_enable_irq(WIFI_NVIC_IRQ);

  // Give lower priority to SYSTICK IRQ than to WIFI USART IRQ so that we can
  // keep pushing data into the ring buffer even when wifi_sys_tick_handler is
  // processing.
  nvic_set_priority(NVIC_SYSTICK_IRQ, (1 << 4));
  nvic_set_priority(WIFI_NVIC_IRQ, (0 << 4));

  usart_enable_rx_interrupt(WIFI_USART);

  usart_enable(WIFI_USART);
}

void wifi_send_string(char *str) {
  while (*str) {
    usart_send_blocking(WIFI_USART, *str++);
  }
}

// wifi_sys_tick_handler consumes the wifi ring buffer and processes it
// according to the current wifi_recv_state. Only one char is processed per
// tick, except when AT_STATUS_FAST_PROCESS is enabled.
void wifi_sys_tick_handler(void) {
  bool status;
  uint8_t data;

process_loop:
  // Temporarily disable interrupts, ring buffer is not thread safe.
  __disable_irq();
  data = ring_buffer_pop(&wifi_rb);
  __enable_irq();

  if (data != '\0') {
    switch (wifi_recv_state) {
      // Asynchronous indications can happen at any point except when an AT
      // command is processing, this is the default wifi_recv_state.
      case recv_async_indication:
        status = wifi_process_async_response(&tmp_buffer[0], data);

        if (status == WIFI_PROCESS_COMPLETE) {
          bool is_wind = wifi_process_wind(&wifi_state, &tmp_buffer[0]);

          if (!is_wind && !wifi_process_bttn_indication(&tmp_buffer[0])) {
            printf("Could not process async response\n");
          }

          memset(&tmp_buffer[0], 0, WIFI_TMP_BUFF_SIZE);
        }

        break;
      // AT command responses only happen after an AT command has been issued,
      // some only return OK / ERROR whereas others have a response body, ending
      // with OK / ERROR.
      case recv_at_response:
        status = wifi_process_at_response(&wifi_at_state, data);
        if (status == WIFI_PROCESS_COMPLETE) {
          wifi_recv_state = recv_async_indication;
        } else if ((wifi_at_state.status & AT_STATUS_FAST_PROCESS) != 0) {
          goto process_loop;
        }
        break;
      // Unhandled recv state.
      default:
        printf("Unknown wifi_recv_state\n");
        break;
    }
  }
}

// WIFI_isr handles interrupts from the WIFI module and stores the data in a
// ring buffer.
void WIFI_isr(void) {
  uint8_t data;
  if (usart_get_flag(WIFI_USART, USART_SR_RXNE)) {
    data = usart_recv(WIFI_USART);

    // Temporarily disable interrupts, ring buffer is not thread safe.
    __disable_irq();
    ring_buffer_push(&wifi_rb, data);
    __enable_irq();
  }
}

// wifi_process_async_response any asynchronous communication from the WIFI
// module, indicating whenever a response is ready to be processed.
bool wifi_process_async_response(uint8_t *const buff, uint8_t data) {
  static uint16_t pos = 0;
  static uint8_t prev = '\0';

  buff[pos++] = data;

  // The beginning and the end of an asynchronous indication is marked by
  // "\r\n", by skipping the first two chars we look for pairs of "\r\n".
  if (pos > 2 && prev == '\r' && data == '\n') {
    wifi_debug_print_buff(buff, '+');

    pos = 0;
    prev = '\0';
    return WIFI_PROCESS_COMPLETE;
  }

  prev = data;
  return WIFI_PROCESS_INCOMPLETE;
}

// wifi_process_at_response processes the data in the provided at buffer and
// indicates whether or not the entire response has been received, the AT status
// is updated accordingly.
bool wifi_process_at_response(wifi_at_t *at, uint8_t data) {
  at->buff[at->pos++] = data;

  // A response must always end at a "\r\n", by skipping len under the minimum
  // response length we avoid unecessary processing in the beginning.
  if (data == '\n' && at->buff[at->pos - 2] == '\r') {
    if (at->last_cr_lf != 0) {
      // Check for AT OK response (end), indicating a successfull HTTP request.
      if (strstr((const char *)at->last_cr_lf, "\r\nOK\r\n")) {
        wifi_debug_print_buff(at->buff, '#');
        at->status = AT_STATUS_OK | AT_STATUS_READY;

        return WIFI_PROCESS_COMPLETE;
      }

      // Check for AT error response (end), indicating there was an error. We
      // check from last_cr_lf to ensure we get the full error message.
      if (strstr((const char *)at->last_cr_lf, "\r\nERROR")) {
        wifi_debug_print_buff(at->buff, '!');
        at->status = AT_STATUS_ERROR | AT_STATUS_READY;

        return WIFI_PROCESS_COMPLETE;
      }
    }

    // Keep track of the current "\r\n" position.
    at->last_cr_lf = &at->buff[at->pos - 2];
  }

  return WIFI_PROCESS_INCOMPLETE;
}

// wifi_process_wind consumes a buffer containing WIND and updates the state if
// a handled WIND ID is found.
bool wifi_process_wind(wifi_state_t *state, uint8_t *const buff_ptr) {
  wifi_wind_t n = wind_undefined;
  char *wind_ptr;

  wind_ptr = strstr((const char *)buff_ptr, "+WIND:");
  if (wind_ptr) {
    wind_ptr += 6;  // Skip over "+WIND:", next char is a digit.

    // We assume the indication ID is never greater than 99 (two digits).
    n = *wind_ptr++ - '0';  // Convert char to int
    if (*wind_ptr != ':') {
      n *= 10;               // First digit was a multiple of 10
      n += *wind_ptr - '0';  // Convert char to int
    }
  }

  switch (n) {
    case wind_power_on:
      // Reset WIFI state after power on.
      *state = WIFI_STATE_POWER_ON;
      break;
    case wind_reset:
      *state = WIFI_STATE_OFF;
      break;
    case wind_console_active:
      *state |= WIFI_STATE_CONSOLE_ACTIVE;
      break;
    case wind_wifi_associated:
      *state |= WIFI_STATE_ASSOCIATED;
      break;
    case wind_wifi_joined:
      *state |= WIFI_STATE_JOINED;
      break;
    case wind_wifi_up:
      *state |= WIFI_STATE_UP;
      break;
    case wind_undefined:
      return false;
      break;
  }

  return true;
}

// wifi_process_bttn_indication consumes a buffer containing BTTN and allows for
// remote controlling of the bttn.
bool wifi_process_bttn_indication(uint8_t *const buff_ptr) {
  wifi_bttn_t n = bttn_undefined;
  char *bttn_ptr;

  bttn_ptr = strstr((const char *)buff_ptr, "+BTTN:");
  if (bttn_ptr) {
    bttn_ptr += 6;  // Skip over "+BTTN:", next char is a digit.

    // We assume the indication ID is never greater than 99 (two digits).
    n = *bttn_ptr++ - '0';  // Convert char to int
    if (*bttn_ptr != ':') {
      n *= 10;               // First digit was a multiple of 10
      n += *bttn_ptr - '0';  // Convert char to int
    }
  }

  switch (n) {
    case bttn_set_url1:
      printf("Set URL1!\n");
      break;
    case bttn_undefined:
      return false;
      break;
  }

  return true;
}

// wifi_wait_state waits until the WIFI module is in specified state.
void wifi_wait_state(wifi_state_t state) {
  while ((wifi_state & state) == 0)
    ;
}

// wifi_at_clear resets the AT command state.
void wifi_at_clear(wifi_at_t *at) {
  memset(at->buff, 0, at->pos);
  at->status = AT_STATUS_CLEAR;
  at->last_cr_lf = 0;
  at->pos = 0;
}

// wifi_at_command_wait waits until we recieve the entire AT response and
// returns true if there was no error, otherwise false.
bool wifi_at_command_wait(void) {
  while ((wifi_at_state.status & AT_STATUS_READY) == 0)
    ;

  return (wifi_at_state.status & AT_STATUS_ERROR) == 0;
}

// wifi_at_command sends an AT command to the WIFI module without blocking, the
// response will still be available in the AT buffer once it is received.
void wifi_at_command(char *str) {
  wifi_wait_state(WIFI_STATE_CONSOLE_ACTIVE);
  wifi_at_clear(&wifi_at_state);

  wifi_send_string(str);
  wifi_send_string("\r");

  wifi_recv_state = recv_at_response;
}

// wifi_at_command_blocking sends an AT command to the WIFI module and blocks
// until it receives an OK or ERROR status. Returns true on OK and false on
// ERROR.
bool wifi_at_command_blocking(char *str) {
  wifi_at_command(str);
  return wifi_at_command_wait();
}

// wifi_http_get_request performs a blocking HTTP GET request and returns the
// http status code returned by the server.
uint16_t wifi_http_get_request(char *url) {
  char req[80];  // TODO: What's the limit???

  sprintf(&req[0], "AT+S.HTTPGET=%s", url);
  wifi_at_command(&req[0]);
  // We use fast processing here to quickly receive the response.
  wifi_at_state.status |= AT_STATUS_FAST_PROCESS;
  wifi_at_command_wait();

  if ((wifi_at_state.status & AT_STATUS_OK) != 0) {
    return wifi_http_parse_status(wifi_at_state.buff);
  } else {
    return 0;
  }
}

// wifi_http_parse_status takes a http response buffer and tries to parse the
// status code from the http header, zero is returned when no status is found.
uint16_t wifi_http_parse_status(uint8_t *response) {
  uint16_t status = 0;
  char status_str[3];
  char *header_ptr;

  header_ptr = strstr((const char *)response, "HTTP/1.");
  if (header_ptr) {
    // The status code is the 9th-11th element of the header.
    // Example: "HTTP/1.0 200 OK"
    memcpy(&status_str, (header_ptr + 9), 3);
    status = atoi(&status_str[0]);
  }

  return status;
}

void wifi_get_ssid(char *dest, size_t len) {
  int i;
  char *s;

  assert(len >= 32);

  wifi_at_command_blocking("AT+S.GCFG=wifi_ssid");

  s = strstr((const char *)wifi_at_state.buff, "#  wifi_ssid = ");
  if (s) {
    s += 15;
    for (i = 0; i < 32 && isxdigit(*s); i++) {  // Max lenght is 32
      dest[i] = strtol(s, &s, 16);
      if (*s == ':') {
        s++;
      }
    }
  }
}

static void wifi_debug_print_buff(uint8_t *const buff, uint8_t prefix) {
  uint8_t *ptr = &buff[0];

  usart_send_blocking(DEBUG_USART, prefix);
  usart_send_blocking(DEBUG_USART, '>');

  while (*ptr) {
    if (*ptr == '\r') {
      usart_send_blocking(DEBUG_USART, '\\');
      usart_send_blocking(DEBUG_USART, 'r');
    } else if (*ptr == '\n') {
      usart_send_blocking(DEBUG_USART, '\\');
      usart_send_blocking(DEBUG_USART, 'n');
    } else {
      usart_send_blocking(DEBUG_USART, *ptr);
    }
    ptr++;
  }

  usart_send_blocking(DEBUG_USART, '\r');
  usart_send_blocking(DEBUG_USART, '\n');
}
