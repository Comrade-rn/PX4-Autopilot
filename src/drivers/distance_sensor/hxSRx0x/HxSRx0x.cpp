/****************************************************************************
 *
 *   Copyright (c) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/


/**
 * @file HxSRx0x.c
 * @author David Sidrane <david.sidrane@nscdg.com>
 *
 * Interface for the HY-SRF05 / HC-SR05 and HC-SR04.
 * Precise Ultrasonic Range Sensor Module
 */

#include "HxSRx0x.h"

#if defined(HAVE_ULTRASOUND)

#include <px4_arch/micro_hal.h>

HxSRx0x::HxSRx0x(const uint8_t rotation) :
	ScheduledWorkItem(MODULE_NAME, px4::wq_configurations::hp_default),
	_px4_rangefinder(0 /* device id not yet used */, rotation)
{
	_px4_rangefinder.set_min_distance(HXSRX0X_MIN_DISTANCE);
	_px4_rangefinder.set_max_distance(HXSRX0X_MAX_DISTANCE);
	_px4_rangefinder.set_fov(0.261799); // 15 degree FOV
}

HxSRx0x::~HxSRx0x()
{
	stop();
	perf_free(_sample_perf);
	perf_free(_comms_errors);
	perf_free(_sensor_resets);
}

void HxSRx0x::OnEdge(bool state)
{
	const hrt_abstime now = hrt_absolute_time();

	if (_state == STATE::WAIT_FOR_RISING || _state == STATE::WAIT_FOR_FALLING) {
		if (state) {
			px4_arch_gpiowrite(GPIO_ULTRASOUND_TRIGGER, 1);
			_rising_edge_time = now;
			_state = STATE::WAIT_FOR_FALLING;

		} else {
			_falling_edge_time = now;
			_state = STATE::SAMPLE;
			ScheduleNow();
		}
	}
}

int HxSRx0x::EchoInterruptCallback(int irq, void *context, void *arg)
{
	static_cast<HxSRx0x *>(arg)->OnEdge(px4_arch_gpioread(GPIO_ULTRASOUND_ECHO));
	return 0;
}

int
HxSRx0x::init()
{
	px4_arch_configgpio(GPIO_ULTRASOUND_TRIGGER);
	px4_arch_configgpio(GPIO_ULTRASOUND_ECHO);
	return PX4_OK;
}

void
HxSRx0x::start()
{
	px4_arch_gpiowrite(GPIO_ULTRASOUND_TRIGGER, 1);
	px4_arch_gpiosetevent(GPIO_ULTRASOUND_ECHO, true, true, false, &EchoInterruptCallback, this);
	_state = STATE::TRIGGER;
	ScheduleOnInterval(get_measure_interval());
}

void
HxSRx0x::stop()
{
	_state = STATE::EXIT;
	px4_arch_gpiosetevent(GPIO_ULTRASOUND_ECHO, false, false, false, nullptr, nullptr);
	px4_arch_gpiowrite(GPIO_ULTRASOUND_TRIGGER, 1);
	ScheduleClear();
}

void
HxSRx0x::Run()
{
	switch (_state) {

	case STATE::TRIGGER:
		_state = STATE::WAIT_FOR_RISING;
		px4_arch_gpiowrite(GPIO_ULTRASOUND_TRIGGER, 0); // ya ya I know they're wrong! It triggers on the falling edge.
		break;

	case STATE::WAIT_FOR_RISING:
	case STATE::WAIT_FOR_FALLING:
		_state = STATE::TRIGGER;
		perf_count(_sensor_resets);
		px4_arch_gpiowrite(GPIO_ULTRASOUND_TRIGGER, 1);
		break;

	case STATE::SAMPLE:
		_state = STATE::MEASURE;
		measure();
		_state = STATE::TRIGGER;
		break;

	case STATE::EXIT:
	default:
		break;
	}
}

int
HxSRx0x::measure()
{
	perf_begin(_sample_perf);

	const hrt_abstime timestamp_sample = hrt_absolute_time();
	const hrt_abstime dt = _falling_edge_time - _rising_edge_time;

	const float current_distance = dt *  343.0f / 10e6f / 2.0f;

	if (dt > HXSRX0X_CONVERSION_TIMEOUT) {
		perf_count(_comms_errors);

	} else {
		_px4_rangefinder.update(timestamp_sample, current_distance);
	}

	perf_end(_sample_perf);
	return PX4_OK;
}

void
HxSRx0x::print_info()
{
	perf_print_counter(_sample_perf);
	perf_print_counter(_comms_errors);
	perf_print_counter(_sensor_resets);
	printf("poll interval:  %u \n", get_measure_interval());
}
#endif
