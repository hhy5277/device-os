/*
 * Copyright (c) 2018 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "interrupts_hal.h"
#include "interrupts_irq.h"
#include "sdk_config_system.h"
#include "nrfx_gpiote.h"
#include "pinmap_impl.h"
#include "logging.h"
#include "nrf_nvic.h"

// 8 high accuracy GPIOTE channels
#define GPIOTE_CHANNEL_NUM              8
// 8 low accuracy port event channels
#define PORT_EVENT_CHANNEL_NUM          NRFX_GPIOTE_CONFIG_NUM_OF_LOW_POWER_EVENTS
// Prioritize the use of high accuracy channels automatically
#define EXIT_CHANNEL_NUM                (GPIOTE_CHANNEL_NUM + PORT_EVENT_CHANNEL_NUM)

static struct {
    uint8_t                 pin;  
    HAL_InterruptCallback   interrupt_callback;
} m_exti_channels[EXIT_CHANNEL_NUM] = {{0}};

extern char link_interrupt_vectors_location;
extern char link_ram_interrupt_vectors_location;
extern char link_ram_interrupt_vectors_location_end;

static void gpiote_interrupt_handler(nrfx_gpiote_pin_t int_nrf_pin, nrf_gpiote_polarity_t action)
{
    NRF5x_Pin_Info* PIN_MAP = HAL_Pin_Map();
    uint8_t src_nrf_pin;

    for (int i = 0; i < EXIT_CHANNEL_NUM; i++)
    {
        if (m_exti_channels[i].pin == PIN_INVALID)
        {
            continue;
        }
        src_nrf_pin = NRF_GPIO_PIN_MAP(PIN_MAP[m_exti_channels[i].pin].gpio_port, PIN_MAP[m_exti_channels[i].pin].gpio_pin);
        if (src_nrf_pin == int_nrf_pin)
        {
            HAL_InterruptHandler user_isr_handle = m_exti_channels[i].interrupt_callback.handler;
            void *data = m_exti_channels[i].interrupt_callback.data;
            if (user_isr_handle)
            {
                user_isr_handle(data);
            }

            break;
        }
    }
}

void HAL_Interrupts_Init(void)
{
    for (int i = 0; i < EXIT_CHANNEL_NUM; i++)
    {
        m_exti_channels[i].pin = PIN_INVALID;
    }
    nrfx_gpiote_init();

    sd_nvic_SetPriority(GPIOTE_IRQn, GPIOTE_CONFIG_IRQ_PRIORITY);
    sd_nvic_ClearPendingIRQ(GPIOTE_IRQn);
    sd_nvic_EnableIRQ(GPIOTE_IRQn);
}

static nrfx_gpiote_in_config_t get_gpiote_config(InterruptMode mode, bool hi_accu)
{
    nrfx_gpiote_in_config_t in_config = {
        .sense = NRF_GPIOTE_POLARITY_TOGGLE,
        .pull = NRF_GPIO_PIN_NOPULL,
        .is_watcher = false,
        .hi_accuracy = hi_accu,
        .skip_gpio_setup = false,
    };

    switch (mode)
    {
        case CHANGE:  
            in_config.sense = NRF_GPIOTE_POLARITY_TOGGLE; 
            in_config.pull = NRF_GPIO_PIN_PULLUP;
            break;
        case RISING:  
            in_config.sense = NRF_GPIOTE_POLARITY_LOTOHI; 
            in_config.pull = NRF_GPIO_PIN_PULLDOWN;
            break;
        case FALLING: 
            in_config.sense = NRF_GPIOTE_POLARITY_HITOLO; 
            in_config.pull = NRF_GPIO_PIN_PULLUP;
            break;
    }

    return in_config;
}

void HAL_Interrupts_Attach(uint16_t pin, HAL_InterruptHandler handler, void* data, InterruptMode mode, HAL_InterruptExtraConfiguration* config)
{
    NRF5x_Pin_Info* PIN_MAP = HAL_Pin_Map();
    uint8_t nrf_pin = NRF_GPIO_PIN_MAP(PIN_MAP[pin].gpio_port, PIN_MAP[pin].gpio_pin);

    nrfx_gpiote_in_config_t in_config;

    in_config = get_gpiote_config(mode, true);
    uint32_t err_code = nrfx_gpiote_in_init(nrf_pin, &in_config, gpiote_interrupt_handler);
    if (err_code == NRFX_ERROR_NO_MEM)
    {
        // High accuracy channels have been used up, use low accuracy channels
        in_config = get_gpiote_config(mode, false);
        err_code = nrfx_gpiote_in_init(nrf_pin, &in_config, gpiote_interrupt_handler);
    }

    if (err_code == NRF_SUCCESS)
    {
        // Add interrupt handler
        for (int i = 0; i < EXIT_CHANNEL_NUM; i++)
        {
            if (m_exti_channels[i].pin == PIN_INVALID)
            {
                m_exti_channels[i].pin = pin;
                m_exti_channels[i].interrupt_callback.handler = handler;
                m_exti_channels[i].interrupt_callback.data = data;

                break;
            }
        }
    }
    else if (err_code == NRFX_ERROR_INVALID_STATE)
    {
        // Change interrupt handler
        if (config->keepHandler == false)
        {
            for (int i = 0; i < EXIT_CHANNEL_NUM; i++)
            {
                if (m_exti_channels[i].pin == pin)
                {
                    m_exti_channels[i].interrupt_callback.handler = handler;
                    m_exti_channels[i].interrupt_callback.data = data;

                    break;
                }
            }
        }
    }
    else if (err_code == NRFX_ERROR_NO_MEM)
    {
        // All channels have been used up
        return;
    }

    PIN_MAP[pin].pin_func = PF_DIO;

    nrfx_gpiote_in_event_enable(nrf_pin, true);
}

void HAL_Interrupts_Detach(uint16_t pin)
{
    HAL_Interrupts_Detach_Ext(pin, 0, NULL);
}

void HAL_Interrupts_Detach_Ext(uint16_t pin, uint8_t keepHandler, void* reserved)
{
    // Just for compatibility
    (void)keepHandler;

    for (int i = 0; i < EXIT_CHANNEL_NUM; i++)
    {
        if (m_exti_channels[i].pin == pin)
        {
            m_exti_channels[i].pin = PIN_INVALID;
            m_exti_channels[i].interrupt_callback.handler = NULL;
            m_exti_channels[i].interrupt_callback.data = NULL;

            break;
        }
    }

    NRF5x_Pin_Info* PIN_MAP = HAL_Pin_Map();
    uint8_t nrf_pin = NRF_GPIO_PIN_MAP(PIN_MAP[pin].gpio_port, PIN_MAP[pin].gpio_pin);
    PIN_MAP[pin].pin_func = PF_NONE;

    nrfx_gpiote_in_event_disable(nrf_pin);
    nrfx_gpiote_in_uninit(nrf_pin);
}

void HAL_Interrupts_Enable_All(void)
{
    sd_nvic_ClearPendingIRQ(GPIOTE_IRQn);
    sd_nvic_EnableIRQ(GPIOTE_IRQn);
}

void HAL_Interrupts_Disable_All(void)
{
    sd_nvic_DisableIRQ(GPIOTE_IRQn);
}

void HAL_Interrupts_Suspend(void)
{
    NRF5x_Pin_Info* PIN_MAP = HAL_Pin_Map();
    uint8_t nrf_pin;

    for (int i = 0; i < EXIT_CHANNEL_NUM; i++)
    {
        if (m_exti_channels[i].pin != PIN_INVALID)
        {
            nrf_pin = NRF_GPIO_PIN_MAP(PIN_MAP[m_exti_channels[i].pin].gpio_port, PIN_MAP[m_exti_channels[i].pin].gpio_pin);
            nrfx_gpiote_in_event_disable(nrf_pin);
        }
    }
}

void HAL_Interrupts_Restore(void)
{
    NRF5x_Pin_Info* PIN_MAP = HAL_Pin_Map();
    uint8_t nrf_pin;

    for (int i = 0; i < EXIT_CHANNEL_NUM; i++)
    {
        if (m_exti_channels[i].pin != PIN_INVALID)
        {
            nrf_pin = NRF_GPIO_PIN_MAP(PIN_MAP[m_exti_channels[i].pin].gpio_port, PIN_MAP[m_exti_channels[i].pin].gpio_pin);
            nrfx_gpiote_in_event_enable(nrf_pin, true);
        }
    }
}

int HAL_Set_Direct_Interrupt_Handler(IRQn_Type irqn, HAL_Direct_Interrupt_Handler handler, uint32_t flags, void* reserved)
{
    if (irqn < NonMaskableInt_IRQn || irqn > I2S_IRQn) 
    {
        return 1;
    }

    int32_t state = HAL_disable_irq();
    volatile uint32_t* isrs = (volatile uint32_t*)&link_ram_interrupt_vectors_location;

    if (handler == NULL && (flags & HAL_DIRECT_INTERRUPT_FLAG_RESTORE)) 
    {
        // Restore
        HAL_Core_Restore_Interrupt(irqn);
    } 
    else 
    {
        isrs[IRQN_TO_IDX(irqn)] = (uint32_t)handler;
    }

    if (flags & HAL_DIRECT_INTERRUPT_FLAG_DISABLE) 
    {
        // Disable
        sd_nvic_DisableIRQ(irqn);
    } 
    else if (flags & HAL_DIRECT_INTERRUPT_FLAG_ENABLE) 
    {
        sd_nvic_EnableIRQ(irqn);
    }

    HAL_enable_irq(state);

    return 0;
}
