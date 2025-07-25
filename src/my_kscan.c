/*
* Copyright (c) 2020-2021 The ZMK Contributors
*
* SPDX-License-Identifier: MIT
*/
#include "kscan_gpio_copy.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/pm/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

#include <zmk/debounce.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_my_kscan

#define INST_DIODE_DIR(n) DT_ENUM_IDX(DT_DRV_INST(n), diode_direction)
#define COND_DIODE_DIR(n, row2col_code, col2row_code)                                              \
    COND_CODE_0(INST_DIODE_DIR(n), row2col_code, col2row_code)

#define INST_ROWS_LEN(n) DT_INST_PROP_LEN(n, row_gpios)
#define INST_COLS_LEN(n) DT_INST_PROP_LEN(n, col_gpios)
#define INST_MATRIX_LEN(n) (2 * INST_ROWS_LEN(n) * INST_COLS_LEN(n))
#define INST_INPUTS_LEN(n) COND_DIODE_DIR(n, (INST_COLS_LEN(n)), (INST_ROWS_LEN(n)))

#ifdef CONFIG_ZMK_MY_KSCAN_DEBOUNCE_PRESS_MS
#define INST_DEBOUNCE_PRESS_MS(n) CONFIG_ZMK_MY_KSCAN_DEBOUNCE_PRESS_MS
#else
#define INST_DEBOUNCE_PRESS_MS(n)                                                                  \
    DT_INST_PROP_OR(n, debounce_period, DT_INST_PROP(n, debounce_press_ms))
#endif

#ifdef CONFIG_ZMK_MY_KSCAN_DEBOUNCE_RELEASE_MS
#define INST_DEBOUNCE_RELEASE_MS(n) CONFIG_ZMK_MY_KSCAN_DEBOUNCE_RELEASE_MS
#else
#define INST_DEBOUNCE_RELEASE_MS(n)                                                                \
    DT_INST_PROP_OR(n, debounce_period, DT_INST_PROP(n, debounce_release_ms))
#endif

#define USE_POLLING IS_ENABLED(CONFIG_ZMK_MY_KSCAN_MATRIX_POLLING)
#define USE_INTERRUPTS (!USE_POLLING)

#define COND_INTERRUPTS(code) COND_CODE_1(CONFIG_ZMK_MY_KSCAN_MATRIX_POLLING, (), code)
#define COND_POLL_OR_INTERRUPTS(pollcode, intcode)                                                 \
    COND_CODE_1(CONFIG_ZMK_MY_KSCAN_MATRIX_POLLING, pollcode, intcode)

#define KSCAN_GPIO_ROW_CFG_INIT(idx, inst_idx)                                                     \
    KSCAN_GPIO_GET_BY_IDX(DT_DRV_INST(inst_idx), row_gpios, idx)
#define KSCAN_GPIO_COL_CFG_INIT(idx, inst_idx)                                                     \
    KSCAN_GPIO_GET_BY_IDX(DT_DRV_INST(inst_idx), col_gpios, idx)
    
enum kscan_diode_direction {
    KSCAN_ROW2COL,
    KSCAN_COL2ROW,
};

struct kscan_matrix_irq_callback {
    const struct device *dev;
    struct gpio_callback callback;
};

struct kscan_matrix_data {
    const struct device *dev;
    struct kscan_gpio_list inputs;
    struct kscan_gpio_list outputs;
    kscan_callback_t callback;
    struct k_work_delayable work;
#if USE_INTERRUPTS
    /** Array of length config->inputs.len */
    struct kscan_matrix_irq_callback *irqs;
#endif
    /** Timestamp of the current or scheduled scan. */
    int64_t scan_time;
    /**
     * Current state of the matrix as a flattened 2D array of length
     * (config->rows * config->cols)
     */
    struct zmk_debounce_state *matrix_state;
};

struct kscan_matrix_config {
    struct zmk_debounce_config debounce_config;
    size_t rows;
    size_t cols;
    int32_t debounce_scan_period_ms;
    int32_t poll_period_ms;
    enum kscan_diode_direction diode_direction;
};


int my_kscan_gpio_pin_get(const struct kscan_gpio *gpio, struct kscan_gpio_port_state *state) {
    if (gpio->spec.port != state->port) {
        state->port = gpio->spec.port;

        const int err = gpio_port_get(state->port, &state->value);
        if (err) {
            return err;
        }
    }

    return (state->value & BIT(gpio->spec.pin)) != 0;
}

#if USE_INTERRUPTS
static int kscan_matrix_interrupt_configure(const struct device *dev, const gpio_flags_t flags) {
    const struct kscan_matrix_data *data = dev->data;

    for (int i = 0; i < data->inputs.len; i++) {
        const struct gpio_dt_spec *gpio = &data->inputs.gpios[i].spec;

        int err = gpio_pin_interrupt_configure_dt(gpio, flags);
        if (err) {
            LOG_ERR("Unable to configure interrupt for pin %u on %s", gpio->pin, gpio->port->name);
            return err;
        }
    }

    return 0;
}
#endif 

#if USE_INTERRUPTS
static int kscan_matrix_interrupt_enable(const struct device *dev) {
    int err = kscan_matrix_interrupt_configure(dev, GPIO_INT_LEVEL_ACTIVE);
    if (err) {
        return err;
    }

    // While interrupts are enabled, set all outputs active so a pressed key
    // will trigger an interrupt.
    return kscan_matrix_set_all_outputs(dev, 1);
}
#endif

#if USE_INTERRUPTS
static int kscan_matrix_interrupt_disable(const struct device *dev) {
    int err = kscan_matrix_interrupt_configure(dev, GPIO_INT_DISABLE);
    if (err) {
        return err;
    }

    // While interrupts are disabled, set all outputs inactive so
    // kscan_matrix_read() can scan them one by one.
    return kscan_matrix_set_all_outputs(dev, 0);
}
#endif

#if USE_INTERRUPTS
static void kscan_matrix_irq_callback_handler(const struct device *port, struct gpio_callback *cb,
                                            const gpio_port_pins_t pin) {
    struct kscan_matrix_irq_callback *irq_data =
        CONTAINER_OF(cb, struct kscan_matrix_irq_callback, callback);
    struct kscan_matrix_data *data = irq_data->dev->data;

    // Disable our interrupts temporarily to avoid re-entry while we scan.
    kscan_matrix_interrupt_disable(data->dev);

    data->scan_time = k_uptime_get();

    k_work_reschedule(&data->work, K_NO_WAIT);
}
#endif

static void kscan_matrix_read_continue(const struct device *dev) {
    const struct kscan_matrix_config *config = dev->config;
    struct kscan_matrix_data *data = dev->data;

    data->scan_time += config->debounce_scan_period_ms;

    k_work_reschedule(&data->work, K_TIMEOUT_ABS_MS(data->scan_time));
}

static void kscan_matrix_read_end(const struct device *dev) {
#if USE_INTERRUPTS
    // Return to waiting for an interrupt.
    kscan_matrix_interrupt_enable(dev);
#else
    struct kscan_matrix_data *data = dev->data;
    const struct kscan_matrix_config *config = dev->config;

    data->scan_time += config->poll_period_ms;

    // Return to polling slowly.
    k_work_reschedule(&data->work, K_TIMEOUT_ABS_MS(data->scan_time));
#endif
}

static int set_pin_as_input(const struct gpio_dt_spec *pin) {
    int err = gpio_pin_configure_dt(pin, GPIO_INPUT | GPIO_PULL_DOWN);
    if (err) {
        LOG_ERR("Unable to configure pin %u on %s for input", pin->pin, pin->port->name);
        return err;
    }

    // LOG_DBG("Configured pin %u on %s for input", pin->pin, pin->port->name);

    return 0;
}

static int kscan_matrix_init_output_inst(const struct device *dev,
                                        const struct gpio_dt_spec *gpio) {
    if (!device_is_ready(gpio->port)) {
        LOG_ERR("GPIO is not ready: %s", gpio->port->name);
        return -ENODEV;
    }

    int err = gpio_pin_configure_dt(gpio, GPIO_OUTPUT_HIGH);
    if (err) {
        LOG_ERR("Unable to configure pin %u on %s for output", gpio->pin, gpio->port->name);
        return err;
    }

    // LOG_DBG("Configured pin %u on %s for output", gpio->pin, gpio->port->name);

    return 0;
}

static int kscan_matrix_read(const struct device *dev) {
    struct kscan_matrix_data *data = dev->data;
    const struct kscan_matrix_config *config = dev->config;
    struct kscan_gpio_list *rows = &data->inputs;
    struct kscan_gpio_list *cols = &data->outputs;

    const size_t MATRIX_COLS = config->cols * 2; // Split keyboard matrix
    LOG_INF("Scanning matrix %s with %zu rows and %zu cols", dev->name, rows->len,
            cols->len);
    // Scan the matrix.
    for (int row = 0; row < rows->len; row++) {
        LOG_INF("Scanning row %i", row);
        const struct kscan_gpio *row_gpio = &rows->gpios[row];
        int err = kscan_matrix_init_output_inst(dev, &row_gpio->spec);
        if (err) {
            LOG_ERR("Failed to set output %i active: %i", row_gpio->index, err);
            return err;
        }

#if CONFIG_MY_KSCAN_MATRIX_WAIT_BEFORE_INPUTS > 0
        k_busy_wait(CONFIG_MY_KSCAN_MATRIX_WAIT_BEFORE_INPUTS);
#endif
        struct kscan_gpio_port_state state = {0};

        for (int col = 0; col < cols->len; col++) {
            const struct kscan_gpio *col_gpio = &cols->gpios[col];

            const int index = MATRIX_COLS * row + col;
            const int active = my_kscan_gpio_pin_get(col_gpio, &state);
            if (active < 0) {
                LOG_ERR("Failed to read port %s: %i", col_gpio->spec.port->name, active);
                return active;
            }
            if (active) {
                LOG_INF("1 - Index %i, row %i, col %i, active %i, pins %d and %d", index, row,
                        col, active, row_gpio->index, col_gpio->index);
            }
            zmk_debounce_update(&data->matrix_state[index], active, config->debounce_scan_period_ms,
                                &config->debounce_config);
        }

        err = set_pin_as_input(&row_gpio->spec);
        if (err) {
            LOG_ERR("Failed to set output %i inactive: %i", row_gpio->index, err);
            return err;
        }

#if CONFIG_MY_KSCAN_MATRIX_WAIT_BETWEEN_OUTPUTS > 0
        k_busy_wait(CONFIG_MY_KSCAN_MATRIX_WAIT_BETWEEN_OUTPUTS);
#endif
    }

    for (int col = 0; col < cols->len; col++) {
        const struct kscan_gpio *col_gpio = &cols->gpios[col];
        int err = kscan_matrix_init_output_inst(dev, &col_gpio->spec);
        if (err) {
            LOG_ERR("Failed to set output %i inactive: %i", col_gpio->index, err);
            return err;
        }

#if CONFIG_MY_KSCAN_MATRIX_WAIT_BEFORE_INPUTS > 0
        k_busy_wait(CONFIG_MY_KSCAN_MATRIX_WAIT_BEFORE_INPUTS);
#endif
        struct kscan_gpio_port_state state = {0};

        for (int row = 0; row < rows->len; row++) {
            const struct kscan_gpio *row_gpio = &rows->gpios[row];

            const int index = MATRIX_COLS * row + (col + MATRIX_COLS / 2);
            const int active = my_kscan_gpio_pin_get(row_gpio, &state);
            if (active < 0) {
                LOG_ERR("Failed to read port %s: %i", row_gpio->spec.port->name, active);
                return active;
            }
            if (active) {
                LOG_INF("2 - Index %i, row %i col %i active %i, pins %d and %d", index, row,
                col + MATRIX_COLS / 2, active, row_gpio->index, col_gpio->index);
            }
            zmk_debounce_update(&data->matrix_state[index], active, config->debounce_scan_period_ms,
                                &config->debounce_config);
        }

        err = set_pin_as_input(&col_gpio->spec);
        if (err) {
            LOG_ERR("Failed to set output %i inactive: %i", col_gpio->index, err);
            return err;
        }

#if CONFIG_MY_KSCAN_MATRIX_WAIT_BETWEEN_OUTPUTS > 0
        k_busy_wait(CONFIG_MY_KSCAN_MATRIX_WAIT_BETWEEN_OUTPUTS);
#endif
    }

    // Process the new state.
    bool continue_scan = false;

    for (int r = 0; r < config->rows; r++) {
        for (int c = 0; c < config->cols * 2; c++) {
            const int index = r * config->cols * 2 + c;
            struct zmk_debounce_state *state = &data->matrix_state[index];

            if (zmk_debounce_get_changed(state)) {
                const bool pressed = zmk_debounce_is_pressed(state);

                LOG_DBG("Sending event at %i,%i state %s", r, c, pressed ? "on" : "off");
                data->callback(dev, r, c, pressed);
            }

            continue_scan = continue_scan || zmk_debounce_is_active(state);
        }
    }

    if (continue_scan) {
        // At least one key is pressed or the debouncer has not yet decided if
        // it is pressed. Poll quickly until everything is released.
        kscan_matrix_read_continue(dev);
    } else {
        // All keys are released. Return to normal.
        kscan_matrix_read_end(dev);
    }

    return 0;
}

static void kscan_matrix_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct kscan_matrix_data *data = CONTAINER_OF(dwork, struct kscan_matrix_data, work);
    kscan_matrix_read(data->dev);
}

static int kscan_matrix_configure(const struct device *dev, const kscan_callback_t callback) {
    struct kscan_matrix_data *data = dev->data;

    if (!callback) {
        return -EINVAL;
    }

    data->callback = callback;
    return 0;
}

static int kscan_matrix_enable(const struct device *dev) {
    struct kscan_matrix_data *data = dev->data;

    data->scan_time = k_uptime_get();

    // Read will automatically start interrupts/polling once done.
    return kscan_matrix_read(dev);
}

static int kscan_matrix_disable(const struct device *dev) {
    struct kscan_matrix_data *data = dev->data;

    k_work_cancel_delayable(&data->work);

#if USE_INTERRUPTS
    return kscan_matrix_interrupt_disable(dev);
#else
    return 0;
#endif
}

static int kscan_matrix_init_input_inst(const struct device *dev, const struct kscan_gpio *gpio) {
    if (!device_is_ready(gpio->spec.port)) {
        LOG_ERR("GPIO is not ready: %s", gpio->spec.port->name);
        return -ENODEV;
    }

    int err = gpio_pin_configure_dt(&gpio->spec, GPIO_INPUT);
    if (err) {
        LOG_ERR("Unable to configure pin %u on %s for input", gpio->spec.pin,
                gpio->spec.port->name);
        return err;
    }

    err = gpio_pin_set_dt(&gpio->spec, 0);
    if (err) {
        LOG_ERR("Unable to set pin %u on %s to low", gpio->spec.pin, gpio->spec.port->name);
        return err;
    }

    // LOG_DBG("Configured pin %u on %s for input", gpio->spec.pin, gpio->spec.port->name);

#if USE_INTERRUPTS
    struct kscan_matrix_data *data = dev->data;
    struct kscan_matrix_irq_callback *irq = &data->irqs[gpio->index];

    irq->dev = dev;
    gpio_init_callback(&irq->callback, kscan_matrix_irq_callback_handler, BIT(gpio->spec.pin));
    err = gpio_add_callback(gpio->spec.port, &irq->callback);
    if (err) {
        LOG_ERR("Error adding the callback to the input device: %i", err);
        return err;
    }
#endif

    return 0;
}

static int kscan_matrix_init_pins(const struct device *dev) {
    const struct kscan_matrix_data *data = dev->data;

    for (int i = 0; i < data->inputs.len; i++) {
        const struct kscan_gpio *gpio = &data->inputs.gpios[i];
        int err = kscan_matrix_init_input_inst(dev, gpio);
        if (err) {
            return err;
        }
    }

    for (int i = 0; i < data->outputs.len; i++) {
        const struct kscan_gpio *gpio = &data->outputs.gpios[i];
        int err = kscan_matrix_init_input_inst(dev, gpio);
        if (err) {
            return err;
        }
    }

    return 0;
}

#if IS_ENABLED(CONFIG_PM_DEVICE)

static int kscan_matrix_disconnect(const struct device *dev) {
    const struct kscan_matrix_data *data = dev->data;

    for (int i = 0; i < data->inputs.len; i++) {
        const struct gpio_dt_spec *gpio = &data->inputs.gpios[i].spec;
        int err = gpio_pin_configure_dt(gpio, GPIO_DISCONNECTED);
        if (err) {
            return err;
        }
    }

    for (int i = 0; i < data->outputs.len; i++) {
    const struct gpio_dt_spec *gpio = &data->outputs.gpios[i].spec;
    int err = gpio_pin_configure_dt(gpio, GPIO_DISCONNECTED);
    if (err) {
        return err;
    }
}

    return 0;
}


#endif // IS_ENABLED(CONFIG_PM_DEVICE)

static void kscan_matrix_setup_pins(const struct device *dev) { kscan_matrix_init_pins(dev); }

static int my_kscan_matrix_init(const struct device *dev) {
    LOG_INF("Initializing kscan matrix %s", dev->name);
    struct kscan_matrix_data *data = dev->data;

    data->dev = dev;

    // Sort inputs by port so we can read each port just once per scan.
    my_kscan_gpio_list_sort_by_port(&data->inputs);

    k_work_init_delayable(&data->work, kscan_matrix_work_handler);

#if IS_ENABLED(CONFIG_PM_DEVICE)
    pm_device_init_suspended(dev);

#if IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)
    pm_device_runtime_enable(dev);
#endif

#else
    kscan_matrix_setup_pins(dev);
#endif

    return 0;
}

#if IS_ENABLED(CONFIG_PM_DEVICE)

static int kscan_matrix_pm_action(const struct device *dev, enum pm_device_action action) {
    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        kscan_matrix_disconnect(dev);

        return kscan_matrix_disable(dev);
    case PM_DEVICE_ACTION_RESUME:
        kscan_matrix_setup_pins(dev);
        return kscan_matrix_enable(dev);
    default:
        return -ENOTSUP;
    }
}

#endif // IS_ENABLED(CONFIG_PM_DEVICE)

static const struct kscan_driver_api kscan_matrix_api = {
    .config = kscan_matrix_configure,
    .enable_callback = kscan_matrix_enable,
    .disable_callback = kscan_matrix_disable,
};

#define MY_KSCAN_MATRIX_INIT(n)                                                                       \
    BUILD_ASSERT(INST_DEBOUNCE_PRESS_MS(n) <= DEBOUNCE_COUNTER_MAX,                                \
                "ZMK_KSCAN_DEBOUNCE_PRESS_MS or debounce-press-ms is too large");                 \
    BUILD_ASSERT(INST_DEBOUNCE_RELEASE_MS(n) <= DEBOUNCE_COUNTER_MAX,                              \
                "ZMK_KSCAN_DEBOUNCE_RELEASE_MS or debounce-release-ms is too large");             \
                                                                                                \
    static struct kscan_gpio kscan_matrix_rows_##n[] = {                                           \
        LISTIFY(INST_ROWS_LEN(n), KSCAN_GPIO_ROW_CFG_INIT, (, ), n)};                              \
                                                                                                \
    static struct kscan_gpio kscan_matrix_cols_##n[] = {                                           \
        LISTIFY(INST_COLS_LEN(n), KSCAN_GPIO_COL_CFG_INIT, (, ), n)};                              \
                                                                                                \
    static struct zmk_debounce_state kscan_matrix_state_##n[INST_MATRIX_LEN(n)];                   \
                                                                                                \
    COND_INTERRUPTS(                                                                               \
        (static struct kscan_matrix_irq_callback kscan_matrix_irqs_##n[INST_INPUTS_LEN(n)];))      \
                                                                                                \
    static struct kscan_matrix_data kscan_matrix_data_##n = {                                      \
        .inputs =                                                                                  \
            KSCAN_GPIO_LIST(kscan_matrix_rows_##n),  \
        .outputs =                                                                                 \
            KSCAN_GPIO_LIST(kscan_matrix_cols_##n),  \
        .matrix_state = kscan_matrix_state_##n,                                                    \
        COND_INTERRUPTS((.irqs = kscan_matrix_irqs_##n, ))};                                       \
                                                                                                \
    static const struct kscan_matrix_config kscan_matrix_config_##n = {                            \
        .rows = ARRAY_SIZE(kscan_matrix_rows_##n),                                                 \
        .cols = ARRAY_SIZE(kscan_matrix_cols_##n),                                                 \
        .debounce_config =                                                                         \
            {                                                                                      \
                .debounce_press_ms = INST_DEBOUNCE_PRESS_MS(n),                                    \
                .debounce_release_ms = INST_DEBOUNCE_RELEASE_MS(n),                                \
            },                                                                                     \
        .debounce_scan_period_ms = DT_INST_PROP(n, debounce_scan_period_ms),                       \
        .poll_period_ms = DT_INST_PROP(n, poll_period_ms),                                         \
        .diode_direction = INST_DIODE_DIR(n),                                                      \
    };                                                                                             \
                                                                                                \
    PM_DEVICE_DT_INST_DEFINE(n, kscan_matrix_pm_action);                                           \
                                                                                                \
    DEVICE_DT_INST_DEFINE(n, &my_kscan_matrix_init, PM_DEVICE_DT_INST_GET(n), &kscan_matrix_data_##n, \
                        &kscan_matrix_config_##n, POST_KERNEL, CONFIG_KSCAN_INIT_PRIORITY,       \
                        &kscan_matrix_api);

DT_INST_FOREACH_STATUS_OKAY(MY_KSCAN_MATRIX_INIT);
