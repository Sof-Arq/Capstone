/*
 * Patient Vital Monitor — Final Integration Capstone
 *
 * Real-time plane (Core 1):
 *   - GPIO 18 button ISR
 *   - Direct-notification patient-alert bottom half
 *   - Binary-semaphore comparison bottom half
 *   - Four periodic background tasks from Application 2
 *
 * Observability plane (Core 0):
 *   - State-of-health reporting
 *   - Heartbeat-based software watchdog checks
 *   - Latency and WCET telemetry output
 */

#ifndef WITH_LOAD
#define WITH_LOAD 1
#endif

#ifndef ENABLE_SEM_COMPARISON
#define ENABLE_SEM_COMPARISON 1
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_err.h"

#define BUTTON_GPIO          GPIO_NUM_18
#define ISR_PULSE_GPIO       GPIO_NUM_19
#define DEBOUNCE_US          200
#define OBS_PERIOD_MS        1000
#define RT_CPU_NUM           APP_CPU_NUM
#define OBS_CPU_NUM          PRO_CPU_NUM

static const char *TAG = "patient_monitor";

/* ISR-to-task IPC objects. */
static SemaphoreHandle_t btn_sem;
static TaskHandle_t task_notif_handle;

/* ISR timestamp is written and consumed on Core 1. */
static volatile int64_t isr_entry_time_us;
static volatile int64_t last_edge_us;

/* Cross-core telemetry uses aligned 32-bit values. */
static volatile uint32_t presses_observed;
static volatile uint32_t nurse_alerts_handled;
static volatile uint32_t sem_events_handled;
static volatile uint32_t latency_last_notif_us;
static volatile uint32_t latency_max_notif_us;
static volatile uint32_t latency_last_sem_us;
static volatile uint32_t latency_max_sem_us;
static volatile uint32_t hb_notif;
static volatile uint32_t hb_sem;

#if WITH_LOAD
static volatile uint32_t hb_a, hb_b, hb_c, hb_d;
static volatile uint32_t wcet_a_max_us, wcet_b_max_us, wcet_c_max_us, wcet_d_max_us;

#define MEASURE_WCET(_max_var, _body) do {                       \
    int64_t _t0 = esp_timer_get_time();                          \
    _body;                                                       \
    uint32_t _dt = (uint32_t)(esp_timer_get_time() - _t0);       \
    if (_dt > (_max_var)) (_max_var) = _dt;                      \
} while (0)
#endif

/* ============================================================
 * Real-time plane — GPIO ISR
 * Producer contract:
 *   Trigger: falling edge on GPIO 18 after the debounce guard.
 *   Produces: one task notification and, when enabled, one binary
 *             semaphore give.
 *   Constraint: bounded, nonblocking, ISR-safe calls only.
 * ============================================================ */
static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();

    if (now - last_edge_us < DEBOUNCE_US) {
        return;
    }
    last_edge_us = now;

    gpio_set_level(ISR_PULSE_GPIO, 1);
    isr_entry_time_us = now;
    presses_observed++;

    BaseType_t higher_woken = pdFALSE;

#if ENABLE_SEM_COMPARISON
    xSemaphoreGiveFromISR(btn_sem, &higher_woken);
#endif
    vTaskNotifyGiveFromISR(task_notif_handle, &higher_woken);

    gpio_set_level(ISR_PULSE_GPIO, 0);
    portYIELD_FROM_ISR(higher_woken);
}

/* ============================================================
 * Direct-notification bottom half — final patient-alert path
 * Consumer contract:
 *   Waits indefinitely for ISR notifications.
 *   Consumes all pending notification counts.
 *   Updates the patient-alert state and latency telemetry.
 * ============================================================ */
static void btn_task_notif(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t count = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (count == 0) {
            continue;
        }

        int64_t wake_us = esp_timer_get_time();
        uint32_t latency_us = (uint32_t)(wake_us - isr_entry_time_us);

        latency_last_notif_us = latency_us;
        if (latency_us > latency_max_notif_us) {
            latency_max_notif_us = latency_us;
        }

        nurse_alerts_handled += count;
        hb_notif++;
    }
}

#if ENABLE_SEM_COMPARISON
/* Measurement-only comparison path retained from Application 3. */
static void btn_task_sem(void *arg)
{
    (void)arg;

    for (;;) {
        if (xSemaphoreTake(btn_sem, portMAX_DELAY) == pdTRUE) {
            int64_t wake_us = esp_timer_get_time();
            uint32_t latency_us = (uint32_t)(wake_us - isr_entry_time_us);

            latency_last_sem_us = latency_us;
            if (latency_us > latency_max_sem_us) {
                latency_max_sem_us = latency_us;
            }

            sem_events_handled++;
            hb_sem++;
        }
    }
}
#endif

#if WITH_LOAD
/* Application 2 periodic load fixture: all tasks are pinned to Core 1. */
#define A_ITERS 100
static volatile uint32_t a_sink;
static void load_task_a(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(10);

    for (;;) {
        MEASURE_WCET(wcet_a_max_us, {
            uint32_t x = a_sink ? a_sink : 0xACE1u;
            for (int i = 0; i < A_ITERS; i++) {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
            }
            a_sink = x;
        });
        hb_a++;
        vTaskDelayUntil(&last, period);
    }
}

#define B_SAMP 16
#define B_TAPS 8
static float b_buf[B_SAMP];
static float b_coef[B_TAPS];
static volatile float b_sink;
static void load_task_b(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(20);

    for (;;) {
        MEASURE_WCET(wcet_b_max_us, {
            float acc = b_sink;
            for (int n = 0; n < B_SAMP; n++) {
                for (int k = 0; k < B_TAPS; k++) {
                    acc += b_buf[(n + B_SAMP - k) & (B_SAMP - 1)] * b_coef[k];
                }
            }
            b_sink = acc;
        });
        hb_b++;
        vTaskDelayUntil(&last, period);
    }
}

#define C_LEN 512
static uint8_t c_pkt[C_LEN];
static volatile uint32_t c_sink;
static void load_task_c(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(50);

    for (;;) {
        MEASURE_WCET(wcet_c_max_us, {
            uint32_t crc = 0xFFFFFFFFu ^ c_sink;
            for (int n = 0; n < C_LEN; n++) {
                crc ^= c_pkt[n];
                for (int b = 0; b < 8; b++) {
                    crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
                }
            }
            c_sink = crc ^ 0xFFFFFFFFu;
        });
        hb_c++;
        vTaskDelayUntil(&last, period);
    }
}

#define D_N 100
static int d_arr[D_N];
static volatile int d_sink;
static void load_task_d(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(100);

    for (;;) {
        MEASURE_WCET(wcet_d_max_us, {
            for (int i = 0; i < D_N; i++) {
                d_arr[i] = D_N - i + (d_sink & 1);
            }
            for (int i = 1; i < D_N; i++) {
                int key = d_arr[i];
                int j = i - 1;
                while (j >= 0 && d_arr[j] > key) {
                    d_arr[j + 1] = d_arr[j];
                    j--;
                }
                d_arr[j + 1] = key;
            }
            d_sink = d_arr[D_N / 2];
        });
        hb_d++;
        vTaskDelayUntil(&last, period);
    }
}

static void load_init_buffers(void)
{
    for (int i = 0; i < B_SAMP; i++) {
        b_buf[i] = (float)((i * 2654435761u) & 0xFFFF) / 65536.0f;
    }
    for (int k = 0; k < B_TAPS; k++) {
        b_coef[k] = 1.0f / (float)B_TAPS;
    }
    for (int n = 0; n < C_LEN; n++) {
        c_pkt[n] = (uint8_t)(n * 31 + 7);
    }
}

static void start_background_load(void)
{
    load_init_buffers();
    xTaskCreatePinnedToCore(load_task_a, "load_a", 2048, NULL, 15, NULL, RT_CPU_NUM);
    xTaskCreatePinnedToCore(load_task_b, "load_b", 2048, NULL, 10, NULL, RT_CPU_NUM);
    xTaskCreatePinnedToCore(load_task_c, "load_c", 2048, NULL, 5, NULL, RT_CPU_NUM);
    xTaskCreatePinnedToCore(load_task_d, "load_d", 2048, NULL, 2, NULL, RT_CPU_NUM);
}
#endif

/* ============================================================
 * Observability plane — Core 0 SOH and software watchdog
 *
 * It reads telemetry once per second. No serial logging occurs in
 * the ISR or the Core 1 bottom-half tasks.
 * ============================================================ */
static void observability_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    uint32_t prev_presses = 0;
    uint32_t prev_notif = 0;
#if ENABLE_SEM_COMPARISON
    uint32_t prev_sem = 0;
#endif
#if WITH_LOAD
    uint32_t prev_a = 0, prev_b = 0, prev_c = 0, prev_d = 0;
#endif
    bool first_sample = true;

    for (;;) {
        uint32_t degraded_mask = 0;

        uint32_t presses = presses_observed;
        uint32_t notif_events = nurse_alerts_handled;
#if ENABLE_SEM_COMPARISON
        uint32_t sem_events = sem_events_handled;
#endif

        if (!first_sample && presses > prev_presses && notif_events == prev_notif) {
            degraded_mask |= (1u << 0);
        }
#if ENABLE_SEM_COMPARISON
        if (!first_sample && presses > prev_presses && sem_events == prev_sem) {
            degraded_mask |= (1u << 1);
        }
#endif

#if WITH_LOAD
        uint32_t a = hb_a, b = hb_b, c = hb_c, d = hb_d;
        if (!first_sample && a == prev_a) degraded_mask |= (1u << 2);
        if (!first_sample && b == prev_b) degraded_mask |= (1u << 3);
        if (!first_sample && c == prev_c) degraded_mask |= (1u << 4);
        if (!first_sample && d == prev_d) degraded_mask |= (1u << 5);
#endif

        ESP_LOGI(TAG,
                 "SOH=%s mask=0x%02lx presses=%lu alerts=%lu notif=%lu/%lu us sem=%lu/%lu us",
                 degraded_mask == 0 ? "OK" : "DEGRADED",
                 (unsigned long)degraded_mask,
                 (unsigned long)presses,
                 (unsigned long)notif_events,
                 (unsigned long)latency_last_notif_us,
                 (unsigned long)latency_max_notif_us,
                 (unsigned long)latency_last_sem_us,
                 (unsigned long)latency_max_sem_us);

#if WITH_LOAD
        ESP_LOGI(TAG,
                 "LOAD hb=[%lu,%lu,%lu,%lu] wcet_max_us=[%lu,%lu,%lu,%lu]",
                 (unsigned long)a, (unsigned long)b,
                 (unsigned long)c, (unsigned long)d,
                 (unsigned long)wcet_a_max_us, (unsigned long)wcet_b_max_us,
                 (unsigned long)wcet_c_max_us, (unsigned long)wcet_d_max_us);
        prev_a = a;
        prev_b = b;
        prev_c = c;
        prev_d = d;
#endif

        prev_presses = presses;
        prev_notif = notif_events;
#if ENABLE_SEM_COMPARISON
        prev_sem = sem_events;
#endif
        first_sample = false;

        vTaskDelayUntil(&last, pdMS_TO_TICKS(OBS_PERIOD_MS));
    }
}

/* Installs the GPIO ISR service from Core 1 so the interrupt and
 * bottom-half response stay on the real-time plane. */
static void rt_setup_task(void *arg)
{
    (void)arg;

    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    gpio_config_t pulse_cfg = {
        .pin_bit_mask = 1ULL << ISR_PULSE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pulse_cfg));
    gpio_set_level(ISR_PULSE_GPIO, 0);

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL));

    ESP_LOGI(TAG, "Real-time plane ready on Core %d: button GPIO %d, pulse GPIO %d",
             RT_CPU_NUM, BUTTON_GPIO, ISR_PULSE_GPIO);

    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    printf("\n==== Patient Vital Monitor capstone starting ====\n");
    printf("Core %d = real-time plane, Core %d = observability plane\n",
           RT_CPU_NUM, OBS_CPU_NUM);
    fflush(stdout);

    ESP_LOGI(TAG, "==== Patient Vital Monitor capstone starting ====");

#if WITH_LOAD
    ESP_LOGI(TAG, "Integrated mode: Application 2 periodic load is enabled");
#else
    ESP_LOGI(TAG, "Idle mode: periodic load is disabled");
#endif

#if ENABLE_SEM_COMPARISON
    btn_sem = xSemaphoreCreateBinary();
    if (btn_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create binary semaphore");
        return;
    }
#endif

    if (xTaskCreatePinnedToCore(btn_task_notif, "patient_alert", 4096, NULL, 12,
                                &task_notif_handle, RT_CPU_NUM) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create patient-alert task");
        return;
    }

#if ENABLE_SEM_COMPARISON
    if (xTaskCreatePinnedToCore(btn_task_sem, "sem_compare", 4096, NULL, 12,
                                NULL, RT_CPU_NUM) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create semaphore comparison task");
        return;
    }
#endif

#if WITH_LOAD
    start_background_load();
#endif

    if (xTaskCreatePinnedToCore(observability_task, "soh_observer", 4096, NULL, 3,
                                NULL, OBS_CPU_NUM) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create observability task");
        return;
    }

    if (xTaskCreatePinnedToCore(rt_setup_task, "rt_setup", 3072, NULL, 14,
                                NULL, RT_CPU_NUM) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create real-time setup task");
        return;
    }
}
