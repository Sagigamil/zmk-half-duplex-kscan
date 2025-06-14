#pragma once

#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

struct kscan_gpio {
    struct gpio_dt_spec spec;
    int index;
};

struct kscan_gpio_list {
    struct kscan_gpio *gpios;
    int len;
};

struct kscan_gpio_port_state {
    const struct device *port;
    gpio_port_value_t value;
};

static int my_compare_ports(const void *a, const void *b) {
    const struct kscan_gpio *gpio_a = a;
    const struct kscan_gpio *gpio_b = b;

    return gpio_a->spec.port - gpio_b->spec.port;
}

void my_kscan_gpio_list_sort_by_port(struct kscan_gpio_list *list) {
    qsort(list->gpios, list->len, sizeof(list->gpios[0]), my_compare_ports);
}

int my_kscan_gpio_pin_get(const struct kscan_gpio *gpio, struct kscan_gpio_port_state *state);

#define KSCAN_GPIO_GET_BY_IDX(node_id, prop, idx)                                                  \
    ((struct kscan_gpio){.spec = GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx), .index = idx})
#define KSCAN_GPIO_LIST(gpio_array)                                                                \
    ((struct kscan_gpio_list){.gpios = gpio_array, .len = ARRAY_SIZE(gpio_array)})
