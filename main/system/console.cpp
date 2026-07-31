#include "console.h"

#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_CONSOLE

#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_console.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "display.h"
#include "heap_monitor.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#endif

namespace {

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
void print_task_table();
#endif

int cmd_free(int argc, char** argv) {
  printf("internal: %" PRIu32 " total: %" PRIu32 "\n",
         esp_get_free_internal_heap_size(), esp_get_free_heap_size());
  return 0;
}

void print_heap_region(const char* name, uint32_t caps) {
  size_t free_bytes = heap_caps_get_free_size(caps);
  size_t min_free = heap_caps_get_minimum_free_size(caps);
  size_t largest = heap_caps_get_largest_free_block(caps);
  printf("%-9s %-10zu %-10zu %-10zu %u%%\n", name, free_bytes, min_free,
         largest, heap_monitor_fragmentation_pct(free_bytes, largest));
}

int cmd_heap(int argc, char** argv) {
  printf("%-9s %-10s %-10s %-10s %s\n", "region", "free", "min", "largest",
         "frag");
  print_heap_region("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  print_heap_region("dma", MALLOC_CAP_DMA);
  print_heap_region("spiram", MALLOC_CAP_SPIRAM);

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
  printf("\n");
  print_task_table();
#endif
  return 0;
}

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
void print_task_table() {
  UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
  auto* task_array = static_cast<TaskStatus_t*>(
      malloc(num_tasks * sizeof(TaskStatus_t)));
  if (!task_array) {
    printf("error: failed to allocate task array\n");
    return;
  }

  uint32_t total_runtime;
  num_tasks = uxTaskGetSystemState(task_array, num_tasks, &total_runtime);

  printf("%-16s %5s %5s %10s\n", "Name", "State", "Prio", "Stack");
  printf("%-16s %5s %5s %10s\n", "----", "-----", "----", "-----");

  for (UBaseType_t i = 0; i < num_tasks; i++) {
    const char* state;
    switch (task_array[i].eCurrentState) {
      case eRunning:
        state = "RUN";
        break;
      case eReady:
        state = "RDY";
        break;
      case eBlocked:
        state = "BLK";
        break;
      case eSuspended:
        state = "SUS";
        break;
      case eDeleted:
        state = "DEL";
        break;
      default:
        state = "???";
        break;
    }
    printf("%-16s %5s %5u %10u\n", task_array[i].pcTaskName, state,
           static_cast<unsigned>(task_array[i].uxCurrentPriority),
           static_cast<unsigned>(task_array[i].usStackHighWaterMark));
  }

  free(task_array);
}

int cmd_task_dump(int argc, char** argv) {
  print_task_table();
  return 0;
}
#endif

int cmd_version(int argc, char** argv) {
  const esp_app_desc_t* app = esp_app_get_description();

  printf("{\n");
  printf("  \"project_name\": \"%s\",\n", app->project_name);
  printf("  \"version\": \"%s\",\n", app->version);
  printf("  \"compile_time\": \"%s\",\n", app->time);
  printf("  \"compile_date\": \"%s\",\n", app->date);
  printf("  \"idf_version\": \"%s\"\n", app->idf_ver);
  printf("}\n");

  return 0;
}

int cmd_assert(int argc, char** argv) {
  printf("Triggering system crash...\n");
  assert(0);
  return 0;
}

// Panel hardware tuning. These exist to diagnose and repair a mis-wired or
// marginal panel over serial without a rebuild, so each prints the current
// value when called with no argument.
int cmd_color_order(int argc, char** argv) {
  if (argc < 2) {
    printf("color_order: %s\nusage: color_order <rgb|bgr>\n",
           display_get_panel_bgr() ? "bgr" : "rgb");
    return 0;
  }

  bool bgr;
  if (strcmp(argv[1], "rgb") == 0) {
    bgr = false;
  } else if (strcmp(argv[1], "bgr") == 0) {
    bgr = true;
  } else {
    printf("invalid color order '%s' (expected rgb or bgr)\n", argv[1]);
    return 1;
  }

  if (!display_set_panel_bgr(bgr)) {
    printf("failed to store color order\n");
    return 1;
  }
  printf("color_order = %s (applies to the next frame)\n", bgr ? "bgr" : "rgb");
  return 0;
}

int cmd_bit_depth(int argc, char** argv) {
  if (argc < 2) {
    printf("bit_depth: %u (0 = compile-time default)\n"
           "usage: bit_depth <4-12>\n",
           display_get_bit_depth());
    return 0;
  }

  int depth = atoi(argv[1]);
  if (depth < 4 || depth > 12) {
    printf("bit depth must be 4-12\n");
    return 1;
  }

  printf("bit_depth = %d, re-initializing display...\n", depth);
  if (!display_set_bit_depth(static_cast<uint8_t>(depth))) {
    printf("failed to apply bit depth\n");
    return 1;
  }
  return 0;
}

int cmd_clock_speed(int argc, char** argv) {
  if (argc < 2) {
    printf("clock_speed: %uMHz\nusage: clock_speed <8|10|16|20|32>\n",
           display_get_clock_mhz());
    return 0;
  }

  int mhz = atoi(argv[1]);
  printf("clock_speed = %dMHz, re-initializing display...\n", mhz);
  if (!display_set_clock_mhz(static_cast<uint8_t>(mhz))) {
    printf("failed to apply clock speed (valid: 8, 10, 16, 20, 32)\n");
    return 1;
  }
  return 0;
}

void register_commands() {
  esp_console_register_help_command();

  const esp_console_cmd_t cmds[] = {
      {.command = "free",
       .help = "Get free heap memory",
       .hint = nullptr,
       .func = &cmd_free,
       .argtable = nullptr,
       .func_w_context = nullptr,
       .context = nullptr},
      {.command = "heap",
       .help =
           "Per-region heap breakdown (free/min/largest/frag) plus task "
           "stacks",
       .hint = nullptr,
       .func = &cmd_heap,
       .argtable = nullptr,
       .func_w_context = nullptr,
       .context = nullptr},
      {.command = "version",
       .help = "Get firmware version information",
       .hint = nullptr,
       .func = &cmd_version,
       .argtable = nullptr,
       .func_w_context = nullptr,
       .context = nullptr},
      {.command = "assert",
       .help = "Crash the system for testing",
       .hint = nullptr,
       .func = &cmd_assert,
       .argtable = nullptr,
       .func_w_context = nullptr,
       .context = nullptr},
      {.command = "color_order",
       .help =
           "Panel color order (rgb|bgr) for R/B-swapped panels, stored in NVS",
       .hint = nullptr,
       .func = &cmd_color_order,
       .argtable = nullptr,
       .func_w_context = nullptr,
       .context = nullptr},
      {.command = "bit_depth",
       .help =
           "Panel color bit depth 4-12, stored in NVS, re-initializes the "
           "display",
       .hint = nullptr,
       .func = &cmd_bit_depth,
       .argtable = nullptr,
       .func_w_context = nullptr,
       .context = nullptr},
      {.command = "clock_speed",
       .help =
           "HUB75 clock in MHz (8|10|16|20|32), stored in NVS, re-initializes "
           "the display",
       .hint = nullptr,
       .func = &cmd_clock_speed,
       .argtable = nullptr,
       .func_w_context = nullptr,
       .context = nullptr},
  };

  for (const auto& cmd : cmds) {
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
  }

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
  const esp_console_cmd_t task_cmd = {
      .command = "task_dump",
      .help = "Print task list (name, state, priority, stack HWM)",
      .hint = nullptr,
      .func = &cmd_task_dump,
      .argtable = nullptr,
      .func_w_context = nullptr,
      .context = nullptr,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&task_cmd));
#endif
}

}  // namespace

void console_init(void) {
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_config.prompt = "tty>";

  register_commands();

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  // USB host may not have enumerated yet at boot — retry briefly.
  bool connected = false;
  for (int i = 0; i < 10; i++) {
    if (usb_serial_jtag_is_connected()) {
      connected = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!connected) {
    return;
  }
  esp_console_dev_usb_serial_jtag_config_t hw_config =
      ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(
      esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
  esp_console_dev_uart_config_t hw_config =
      ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(
      esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#endif
  ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

#else  // !CONFIG_ENABLE_CONSOLE

void console_init(void) {}

#endif
