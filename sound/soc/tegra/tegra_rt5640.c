/*
 * tegra_rt5640.c - Tegra machine ASoC driver for boards using ALC5640 codec.
 *
 * Author: Johnny Qiu <joqiu@nvidia.com>
 * Copyright (C) 2011-2013, NVIDIA, Inc.
 *
 * Based on code copyright/by:
 *
 * Copyright 2007 Wolfson Microelectronics PLC.
 * Author: Graeme Gregory
 *         graeme.gregory@wolfsonmicro.com or linux@wolfsonmicro.com
 *
 * Copyright (c) 2012-2013, NVIDIA CORPORATION. All rights reserved.
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <asm/mach-types.h>

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif
#include <mach/tegra_asoc_pdata.h>
#include <mach/gpio-tegra.h>
#include <mach/tegra_rt5640_pdata.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "../codecs/rt5639.h"
#include "../codecs/rt5640.h"

#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"
#include <linux/tfa9887.h>
#include "tegra30_ahub.h"
#include "tegra30_i2s.h"

#define DRV_NAME "tegra-snd-rt5640"

#define GPIO_SPKR_EN    BIT(0)
#define GPIO_HP_MUTE    BIT(1)
#define GPIO_INT_MIC_EN BIT(2)
#define GPIO_EXT_MIC_EN BIT(3)
#define GPIO_HP_DET     BIT(4)

struct tegra30_i2s *i2s_tfa = NULL;
struct snd_soc_codec *codec_rt;

struct tegra_rt5640 {
	struct tegra_asoc_utils_data util_data;
	struct tegra_asoc_platform_data *pdata;
	struct regulator *spk_reg;
	struct regulator *dmic_reg;
	struct regulator *cdc_en;
	int gpio_requested;
#ifdef CONFIG_SWITCH
	int jack_status;
#endif
	enum snd_soc_bias_level bias_level;
	volatile int clock_enabled;
	int speaker_sel;
	int tfa9887_on;
};

void tegra_asoc_enable_clocks(void);
void tegra_asoc_disable_clocks(void);

static int tegra_rt5640_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(cpu_dai);
	int srate, mclk, i2s_daifmt;
	int err, rate;
	static unsigned int initTfa;
	static int tfasrate;

	srate = params_rate(params);
	mclk = 256 * srate;

	i2s_daifmt = SND_SOC_DAIFMT_NB_NF;
	i2s_daifmt |= pdata->i2s_param[HIFI_CODEC].is_i2s_master ?
			SND_SOC_DAIFMT_CBS_CFS : SND_SOC_DAIFMT_CBM_CFM;

	switch (pdata->i2s_param[HIFI_CODEC].i2s_mode) {
		case TEGRA_DAIFMT_I2S :
			i2s_daifmt |= SND_SOC_DAIFMT_I2S;
			break;
		case TEGRA_DAIFMT_DSP_A :
			i2s_daifmt |= SND_SOC_DAIFMT_DSP_A;
			break;
		case TEGRA_DAIFMT_DSP_B :
			i2s_daifmt |= SND_SOC_DAIFMT_DSP_B;
			break;
		case TEGRA_DAIFMT_LEFT_J :
			i2s_daifmt |= SND_SOC_DAIFMT_LEFT_J;
			break;
		case TEGRA_DAIFMT_RIGHT_J :
			i2s_daifmt |= SND_SOC_DAIFMT_RIGHT_J;
			break;
		default :
			dev_err(card->dev, "Can't configure i2s format\n");
			return -EINVAL;
	}

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % mclk)) {
			mclk = machine->util_data.set_mclk;
		} else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	rate = clk_get_rate(machine->util_data.clk_cdev1);

	err = snd_soc_dai_set_fmt(codec_dai, i2s_daifmt);
	if (err < 0) {
		dev_err(card->dev, "codec_dai fmt not set\n");
		return err;
	}

	err = snd_soc_dai_set_fmt(cpu_dai, i2s_daifmt);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai fmt not set\n");
		return err;
	}

	err = snd_soc_dai_set_sysclk(codec_dai, 0, rate, SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev, "codec_dai clock not set\n");
		return err;
	}
	if (machine_is_roth()) {
		if (initTfa == 1) {
			i2s_tfa = i2s;
			tegra_asoc_enable_clocks();
			pr_info("INIT TFA\n");
			Tfa9887_Init(srate);
			tfasrate = srate;
			tegra_asoc_disable_clocks();
		} else if (initTfa > 1) {
			if (srate != tfasrate) {
				Tfa9887_setSampleRate(srate);
				tfasrate = srate;
			}
		}
		initTfa++;
	}
	return 0;
}

static int tegra_bt_sco_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	int srate, mclk, min_mclk, i2s_daifmt;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		mclk = 11289600;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
		mclk = 12288000;
		break;
	default:
		return -EINVAL;
	}
	min_mclk = 64 * srate;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % min_mclk))
			mclk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	i2s_daifmt = SND_SOC_DAIFMT_NB_NF;
	i2s_daifmt |= pdata->i2s_param[BT_SCO].is_i2s_master ?
			SND_SOC_DAIFMT_CBS_CFS : SND_SOC_DAIFMT_CBM_CFM;

	switch (pdata->i2s_param[BT_SCO].i2s_mode) {
		case TEGRA_DAIFMT_I2S :
			i2s_daifmt |= SND_SOC_DAIFMT_I2S;
			break;
		case TEGRA_DAIFMT_DSP_A :
			i2s_daifmt |= SND_SOC_DAIFMT_DSP_A;
			break;
		case TEGRA_DAIFMT_DSP_B :
			i2s_daifmt |= SND_SOC_DAIFMT_DSP_B;
			break;
		case TEGRA_DAIFMT_LEFT_J :
			i2s_daifmt |= SND_SOC_DAIFMT_LEFT_J;
			break;
		case TEGRA_DAIFMT_RIGHT_J :
			i2s_daifmt |= SND_SOC_DAIFMT_RIGHT_J;
			break;
		default :
			dev_err(card->dev, "Can't configure i2s format\n");
			return -EINVAL;
	}

	err = snd_soc_dai_set_fmt(rtd->cpu_dai, i2s_daifmt);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai fmt not set\n");
		return err;
	}

	return 0;
}

static int tegra_spdif_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	int srate, mclk, min_mclk;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		mclk = 11289600;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
		mclk = 12288000;
		break;
	default:
		return -EINVAL;
	}
	min_mclk = 128 * srate;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % min_mclk))
			mclk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	return 0;
}

static int tegra_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(rtd->card);

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 0);

	return 0;
}

static struct snd_soc_ops tegra_rt5640_ops = {
	.hw_params = tegra_rt5640_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_ops tegra_rt5640_bt_sco_ops = {
	.hw_params = tegra_bt_sco_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_ops tegra_spdif_ops = {
	.hw_params = tegra_spdif_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_jack tegra_rt5640_hp_jack;

static struct snd_soc_jack_gpio tegra_rt5640_hp_jack_gpio = {
	.name = "headphone detect",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 150,
	.invert = 1,
};

#ifdef CONFIG_SWITCH
/* These values are copied from Android WiredAccessoryObserver */
enum headset_state {
	BIT_NO_HEADSET = 0,
	BIT_HEADSET = (1 << 0),
	BIT_HEADSET_NO_MIC = (1 << 1),
};

static struct switch_dev tegra_rt5640_headset_switch = {
	.name = "h2w",
};

static int tegra_rt5640_jack_notifier(struct notifier_block *self,
			      unsigned long action, void *dev)
{
	struct snd_soc_jack *jack = dev;
	struct snd_soc_codec *codec = jack->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	enum headset_state state = BIT_NO_HEADSET;
	unsigned char status_jack = 0;

	if (jack == &tegra_rt5640_hp_jack) {
		if (action) {
			/* Enable ext mic; enable signal is active-low */
			gpio_direction_output(pdata->gpio_ext_mic_en, 0);
			if (!strncmp(machine->pdata->codec_name, "rt5639", 6))
				status_jack = rt5639_headset_detect(codec, 1);
			else if (!strncmp(machine->pdata->codec_name, "rt5640",
									    6))
				status_jack = rt5640_headset_detect(codec, 1);

			machine->jack_status &= ~SND_JACK_HEADPHONE;
			machine->jack_status &= ~SND_JACK_MICROPHONE;
			if (status_jack == RT5639_HEADPHO_DET ||
			    status_jack == RT5640_HEADPHO_DET)
					machine->jack_status |=
							SND_JACK_HEADPHONE;
			else if (status_jack == RT5639_HEADSET_DET ||
				 status_jack == RT5640_HEADSET_DET) {
					machine->jack_status |=
							SND_JACK_HEADPHONE;
					machine->jack_status |=
							SND_JACK_MICROPHONE;
			}
		} else {
			/* Disable ext mic; enable signal is active-low */
			gpio_direction_output(pdata->gpio_ext_mic_en, 1);
			if (!strncmp(machine->pdata->codec_name, "rt5639", 6))
				rt5639_headset_detect(codec, 0);
			else if (!strncmp(machine->pdata->codec_name, "rt5640",
									    6))
				rt5640_headset_detect(codec, 0);

			machine->jack_status &= ~SND_JACK_HEADPHONE;
			machine->jack_status &= ~SND_JACK_MICROPHONE;
		}
	}

	switch (machine->jack_status) {
	case SND_JACK_HEADPHONE:
		state = BIT_HEADSET_NO_MIC;
		break;
	case SND_JACK_HEADSET:
		state = BIT_HEADSET;
		break;
	case SND_JACK_MICROPHONE:
		/* mic: would not report */
	default:
		state = BIT_NO_HEADSET;
	}

	switch_set_state(&tegra_rt5640_headset_switch, state);

	return NOTIFY_OK;
}

static struct notifier_block tegra_rt5640_jack_detect_nb = {
	.notifier_call = tegra_rt5640_jack_notifier,
};
#else
static struct snd_soc_jack_pin tegra_rt5640_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

#endif

static int tegra_rt5640_event_int_spk(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;

	if (machine->spk_reg) {
		if (SND_SOC_DAPM_EVENT_ON(event)) {
			regulator_enable(machine->spk_reg);
		}
		else {
			regulator_disable(machine->spk_reg);
		}
	}

	if (machine_is_roth()) {
		if (i2s_tfa) {
			if (SND_SOC_DAPM_EVENT_ON(event)) {
			    machine->speaker_sel = 1;
				if (!machine->tfa9887_on) {
					if (codec_rt)
						snd_soc_update_bits(codec_rt,
							RT5640_PWR_DIG1, 0x0001, 0x0000);
					tegra_asoc_enable_clocks();
					Tfa9887_Powerdown(0);
					tegra_asoc_disable_clocks();
					machine->tfa9887_on = 1;
				}
			} else {
				Tfa9887_Powerdown(1);
				machine->tfa9887_on = 0;
			}
		}
	}

	if (!(machine->gpio_requested & GPIO_SPKR_EN))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_spkr_en,
				!!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int tegra_rt5640_event_hp(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;

	machine->speaker_sel = 0;
	if (machine_is_roth()) {
		if (i2s_tfa) {
			if (SND_SOC_DAPM_EVENT_ON(event)) {
				Tfa9887_Powerdown(1);
				machine->tfa9887_on = 0;
			}
		}
	}

	if (!(machine->gpio_requested & GPIO_HP_MUTE))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_hp_mute,
				!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int tegra_rt5640_event_int_mic(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;

	if (machine->dmic_reg) {
		if (SND_SOC_DAPM_EVENT_ON(event))
			regulator_enable(machine->dmic_reg);
		else
			regulator_disable(machine->dmic_reg);
	}

	if (!(machine->gpio_requested & GPIO_INT_MIC_EN))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_int_mic_en,
				!!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int tegra_rt5640_event_ext_mic(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;

	if (!(machine->gpio_requested & GPIO_EXT_MIC_EN))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_ext_mic_en,
				!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static const struct snd_soc_dapm_widget cardhu_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Int Spk", tegra_rt5640_event_int_spk),
	SND_SOC_DAPM_HP("Headphone Jack", tegra_rt5640_event_hp),
	SND_SOC_DAPM_MIC("Mic Jack", tegra_rt5640_event_ext_mic),
	SND_SOC_DAPM_MIC("Int Mic", tegra_rt5640_event_int_mic),
};

static const struct snd_soc_dapm_route cardhu_audio_map[] = {
	{"Headphone Jack", NULL, "HPOR"},
	{"Headphone Jack", NULL, "HPOL"},
	{"Int Spk", NULL, "SPORP"},
	{"Int Spk", NULL, "SPORN"},
	{"Int Spk", NULL, "SPOLP"},
	{"Int Spk", NULL, "SPOLN"},
	{"micbias1", NULL, "Mic Jack"},
	{"IN2P", NULL, "micbias1"},
	{"DMIC L1", NULL, "Int Mic"},
	{"DMIC L2", NULL, "Int Mic"},
	{"DMIC R1", NULL, "Int Mic"},
	{"DMIC R2", NULL, "Int Mic"},
};

static const struct snd_kcontrol_new cardhu_controls[] = {
	SOC_DAPM_PIN_SWITCH("Int Spk"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Mic Jack"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
};

static int tegra_set_tfa9887_powerdown(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct  tegra_asoc_utils_data *data = snd_kcontrol_chip(kcontrol);
	struct snd_soc_card *card = data->card;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);

	data->tfa9887_powerdown = ucontrol->value.integer.value[0];
	if (i2s_tfa && (i2s_tfa->playback_ref_count <= 1)) {
		Tfa9887_Powerdown(data->tfa9887_powerdown);
		machine->tfa9887_on = !data->tfa9887_powerdown;
	}
	return 1;
}

static int tegra_get_tfa9887_powerdown(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct  tegra_asoc_utils_data *data = snd_kcontrol_chip(kcontrol);
	ucontrol->value.integer.value[0] = data->tfa9887_powerdown;

	return 0;
}

struct snd_kcontrol_new tegra_tfa9887_controls =
	SOC_SINGLE_EXT("tfa9887 powerdown", 0, 0, 1, \
	0, tegra_get_tfa9887_powerdown, tegra_set_tfa9887_powerdown);


static int tegra_rt5640_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = codec->card;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	int ret;

	codec_rt = codec;

	if (gpio_is_valid(pdata->gpio_spkr_en)) {
		ret = gpio_request(pdata->gpio_spkr_en, "spkr_en");
		if (ret) {
			dev_err(card->dev, "cannot get spkr_en gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_SPKR_EN;

		gpio_direction_output(pdata->gpio_spkr_en, 0);
	}

	if (gpio_is_valid(pdata->gpio_hp_mute)) {
		ret = gpio_request(pdata->gpio_hp_mute, "hp_mute");
		if (ret) {
			dev_err(card->dev, "cannot get hp_mute gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_HP_MUTE;

		gpio_direction_output(pdata->gpio_hp_mute, 0);
	}

	if (gpio_is_valid(pdata->gpio_int_mic_en)) {
		ret = gpio_request(pdata->gpio_int_mic_en, "int_mic_en");
		if (ret) {
			dev_err(card->dev, "cannot get int_mic_en gpio\n");
		} else {
			machine->gpio_requested |= GPIO_INT_MIC_EN;

			/* Disable int mic; enable signal is active-high */
			gpio_direction_output(pdata->gpio_int_mic_en, 0);
		}
	}

	if (gpio_is_valid(pdata->gpio_ext_mic_en)) {
		ret = gpio_request(pdata->gpio_ext_mic_en, "ext_mic_en");
		if (ret) {
			dev_err(card->dev, "cannot get ext_mic_en gpio\n");
		} else {
			machine->gpio_requested |= GPIO_EXT_MIC_EN;

			/* Disable ext mic; enable signal is active-low */
			gpio_direction_output(pdata->gpio_ext_mic_en, 1);
		}
	}

	if (gpio_is_valid(pdata->gpio_hp_det)) {
		tegra_rt5640_hp_jack_gpio.gpio = pdata->gpio_hp_det;
		snd_soc_jack_new(codec, "Headphone Jack", SND_JACK_HEADPHONE,
				&tegra_rt5640_hp_jack);
#ifndef CONFIG_SWITCH
		snd_soc_jack_add_pins(&tegra_rt5640_hp_jack,
					ARRAY_SIZE(tegra_rt5640_hp_jack_pins),
					tegra_rt5640_hp_jack_pins);
#else
		snd_soc_jack_notifier_register(&tegra_rt5640_hp_jack,
					&tegra_rt5640_jack_detect_nb);
#endif
		snd_soc_jack_add_gpios(&tegra_rt5640_hp_jack,
					1,
					&tegra_rt5640_hp_jack_gpio);
		machine->gpio_requested |= GPIO_HP_DET;
	}

	ret = tegra_asoc_utils_register_ctls(&machine->util_data);
	if (ret < 0)
		return ret;

	if (machine_is_roth()) {
		ret = snd_ctl_add(machine->util_data.card->snd_card,
			snd_ctl_new1(&tegra_tfa9887_controls, &machine->util_data));
		if (ret < 0) {
			dev_err(card->dev, "Can't add tfa9887 alsa control");
			return ret;
		}
	}

	/* FIXME: Calculate automatically based on DAPM routes? */
	snd_soc_dapm_nc_pin(dapm, "LOUTL");
	snd_soc_dapm_nc_pin(dapm, "LOUTR");

	snd_soc_dapm_sync(dapm);

	return 0;
}

static struct snd_soc_dai_link tegra_rt5640_dai[] = {
	{
		.name = "RT5640",
		.stream_name = "RT5640 PCM",
		.codec_name = "rt5640.4-001c",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra30-i2s.1",
		.codec_dai_name = "rt5640-aif1",
		.init = tegra_rt5640_init,
		.ops = &tegra_rt5640_ops,
	},
	{
		.name = "SPDIF",
		.stream_name = "SPDIF PCM",
		.codec_name = "spdif-dit.0",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra30-spdif",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_spdif_ops,
	},
	{
		.name = "BT-SCO",
		.stream_name = "BT SCO PCM",
		.codec_name = "spdif-dit.1",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra30-i2s.3",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_rt5640_bt_sco_ops,
	},
};

static int tegra_rt5640_resume_pre(struct snd_soc_card *card)
{
	int val;
	struct snd_soc_jack_gpio *gpio = &tegra_rt5640_hp_jack_gpio;

	if (gpio_is_valid(gpio->gpio)) {
		val = gpio_get_value(gpio->gpio);
		val = gpio->invert ? !val : val;
		snd_soc_jack_report(gpio->jack, val, gpio->report);
	}

	return 0;
}

static int tegra_rt5640_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);

	if (machine->bias_level == SND_SOC_BIAS_OFF &&
		level != SND_SOC_BIAS_OFF && (!machine->clock_enabled)) {
		machine->clock_enabled = 1;
		tegra_asoc_utils_clk_enable(&machine->util_data);
		machine->bias_level = level;
	}

    if (machine_is_roth()) {
	    if (machine->speaker_sel &&
		    !machine->tfa9887_on &&
		    level > machine->bias_level) {
		    if (i2s_tfa) {
			    if (codec_rt)
				    snd_soc_update_bits(codec_rt, RT5640_PWR_DIG1, 0x0001, 0x0000);
			    tegra_asoc_enable_clocks();
			    Tfa9887_Powerdown(0);
			    tegra_asoc_disable_clocks();
			    machine->tfa9887_on = 1;
		    }
	    }
		if (level == SND_SOC_BIAS_OFF) {
			if (i2s_tfa) {
				Tfa9887_Powerdown(1);
				machine->tfa9887_on = 0;
			}
		}
	}
	return 0;
}

static int tegra_rt5640_set_bias_level_post(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	//printk("tegra_rt5640_set_bias_level_post level =%d\n",level);
	if (machine->bias_level != SND_SOC_BIAS_OFF &&
		level == SND_SOC_BIAS_OFF && machine->clock_enabled) {
		machine->clock_enabled = 0;
		tegra_asoc_utils_clk_disable(&machine->util_data);
	}

	machine->bias_level = level;

	return 0 ;
}

static struct snd_soc_card snd_soc_tegra_rt5640 = {
	.name = "tegra-rt5640",
	.owner = THIS_MODULE,
	.dai_link = tegra_rt5640_dai,
	.num_links = ARRAY_SIZE(tegra_rt5640_dai),
	.resume_pre = tegra_rt5640_resume_pre,
	.set_bias_level = tegra_rt5640_set_bias_level,
	.set_bias_level_post = tegra_rt5640_set_bias_level_post,
	.controls = cardhu_controls,
	.num_controls = ARRAY_SIZE(cardhu_controls),
	.dapm_widgets = cardhu_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cardhu_dapm_widgets),
	.dapm_routes = cardhu_audio_map,
	.num_dapm_routes = ARRAY_SIZE(cardhu_audio_map),
	.fully_routed = true,
};

void tegra_asoc_enable_clocks()
{
	struct snd_soc_card *card = &snd_soc_tegra_rt5640;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	int reg;
	struct tegra30_i2s *i2s = i2s_tfa;
	if (!i2s || !machine)
		return;

	reg = i2s->reg_ctrl | TEGRA30_I2S_CTRL_XFER_EN_TX;
	if (!(i2s->reg_ctrl & TEGRA30_I2S_CTRL_XFER_EN_TX)) {
		tegra_asoc_utils_clk_enable(&machine->util_data);
		clk_enable(i2s->clk_i2s);
		tegra30_ahub_enable_clocks();
		tegra30_ahub_enable_tx_fifo(i2s->txcif);
		__raw_writel(reg, i2s->regs + TEGRA30_I2S_CTRL);
	}
}
EXPORT_SYMBOL_GPL(tegra_asoc_enable_clocks);

void tegra_asoc_disable_clocks()
{
	struct snd_soc_card *card = &snd_soc_tegra_rt5640;
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	int dcnt = 10;
	struct tegra30_i2s *i2s = i2s_tfa;
	if (!i2s || !machine)
		return;

	if (!(i2s->reg_ctrl & TEGRA30_I2S_CTRL_XFER_EN_TX)) {
		__raw_writel(i2s->reg_ctrl, i2s->regs + TEGRA30_I2S_CTRL);
		while (!tegra30_ahub_tx_fifo_is_empty(i2s->id) && dcnt--)
			udelay(100);

		tegra30_ahub_disable_tx_fifo(i2s->txcif);
		tegra30_ahub_disable_clocks();
		clk_disable(i2s->clk_i2s);
		tegra_asoc_utils_clk_disable(&machine->util_data);
	}
}
EXPORT_SYMBOL_GPL(tegra_asoc_disable_clocks);


static __devinit int tegra_rt5640_driver_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_tegra_rt5640;
	struct tegra_rt5640 *machine;
	struct tegra_asoc_platform_data *pdata;
	int ret;

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}
	if (pdata->codec_name)
		card->dai_link->codec_name = pdata->codec_name;

	if (pdata->codec_dai_name)
		card->dai_link->codec_dai_name = pdata->codec_dai_name;

	machine = kzalloc(sizeof(struct tegra_rt5640), GFP_KERNEL);
	if (!machine) {
		dev_err(&pdev->dev, "Can't allocate tegra_rt5640 struct\n");
		return -ENOMEM;
	}

	if (gpio_is_valid(pdata->gpio_ldo1_en)) {
		ret = gpio_request(pdata->gpio_ldo1_en, "rt5640");
		if (ret) {
			dev_err(&pdev->dev, "Fail gpio_request AUDIO_LDO1\n");
		}

		ret = gpio_direction_output(pdata->gpio_ldo1_en, 1);
		if (ret)
			dev_err(&pdev->dev, "Fail gpio_direction AUDIO_LDO1\n");

		msleep(200);
	}

	if (gpio_is_valid(pdata->gpio_codec1)) {
		ret = gpio_request(pdata->gpio_codec1, "rt5640");
		if (ret)
			dev_err(&pdev->dev, "Fail gpio_request GPIO_CODEC1\n");

		ret = gpio_direction_output(pdata->gpio_codec1, 1);
		if (ret)
			dev_err(&pdev->dev, "Fail gpio_direction GPIO_CODEC1\n");

		msleep(200);
	}

	if (gpio_is_valid(pdata->gpio_codec2)) {
		ret = gpio_request(pdata->gpio_codec2, "rt5640");
		if (ret)
			dev_err(&pdev->dev, "Fail gpio_request GPIO_CODEC2\n");

		ret = gpio_direction_output(pdata->gpio_codec2, 1);
		if (ret)
			dev_err(&pdev->dev, "Fail gpio_direction GPIO_CODEC2\n");

		msleep(200);
	}

	if (gpio_is_valid(pdata->gpio_codec3)) {
		ret = gpio_request(pdata->gpio_codec3, "rt5640");
		if (ret)
			dev_err(&pdev->dev, "Fail gpio_request GPIO_CODEC3\n");

		ret = gpio_direction_output(pdata->gpio_codec3, 1);
		if (ret)
			dev_err(&pdev->dev, "Fail gpio_direction GPIO_CODEC3\n");

		msleep(200);
	}

	machine->pdata = pdata;

	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev, card);
	if (ret)
		goto err_free_machine;

	machine->bias_level = SND_SOC_BIAS_STANDBY;
	machine->clock_enabled = 1;

	if (!gpio_is_valid(pdata->gpio_ldo1_en)) {
		machine->cdc_en = regulator_get(&pdev->dev, "ldo1_en");
		if (IS_ERR(machine->cdc_en)) {
			dev_err(&pdev->dev, "ldo1_en regulator not found %ld\n",
					PTR_ERR(machine->cdc_en));
			machine->cdc_en = 0;
		} else {
			regulator_enable(machine->cdc_en);
		}
	}

	machine->spk_reg = regulator_get(&pdev->dev, "vdd_spk");
	if (IS_ERR(machine->spk_reg)) {
		dev_info(&pdev->dev, "No speaker regulator found\n");
		machine->spk_reg = 0;
	}

#ifdef CONFIG_SWITCH
	/* Addd h2w swith class support */
	ret = tegra_asoc_switch_register(&tegra_rt5640_headset_switch);
	if (ret < 0)
		goto err_fini_utils;
#endif

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, machine);
	card->dapm.idle_bias_off = 1;
	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err_unregister_switch;
	}

	if (!card->instantiated) {
		ret = -ENODEV;
		dev_err(&pdev->dev, "sound card not instantiated (%d)\n",
			ret);
		goto err_unregister_card;
	}

#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	ret = tegra_asoc_utils_set_parent(&machine->util_data,
				pdata->i2s_param[HIFI_CODEC].is_i2s_master);
	if (ret) {
		dev_err(&pdev->dev, "tegra_asoc_utils_set_parent failed (%d)\n",
			ret);
		goto err_unregister_card;
	}
#endif

	return 0;

err_unregister_card:
	snd_soc_unregister_card(card);
err_unregister_switch:
#ifdef CONFIG_SWITCH
	tegra_asoc_switch_unregister(&tegra_rt5640_headset_switch);
err_fini_utils:
#endif
	tegra_asoc_utils_fini(&machine->util_data);
err_free_machine:
	kfree(machine);
	return ret;
}

static int __devexit tegra_rt5640_driver_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_rt5640 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;

	if (machine->gpio_requested & GPIO_HP_DET)
		snd_soc_jack_free_gpios(&tegra_rt5640_hp_jack,
					1,
					&tegra_rt5640_hp_jack_gpio);
	if (machine->gpio_requested & GPIO_EXT_MIC_EN)
		gpio_free(pdata->gpio_ext_mic_en);
	if (machine->gpio_requested & GPIO_INT_MIC_EN)
		gpio_free(pdata->gpio_int_mic_en);
	if (machine->gpio_requested & GPIO_HP_MUTE)
		gpio_free(pdata->gpio_hp_mute);
	if (machine->gpio_requested & GPIO_SPKR_EN)
		gpio_free(pdata->gpio_spkr_en);
	machine->gpio_requested = 0;

	if (machine->spk_reg)
		regulator_put(machine->spk_reg);
	if (machine->dmic_reg)
		regulator_put(machine->dmic_reg);

	if (machine->cdc_en) {
		regulator_disable(machine->cdc_en);
		regulator_put(machine->cdc_en);
	}

	if (gpio_is_valid(pdata->gpio_ldo1_en)) {
		gpio_set_value(pdata->gpio_ldo1_en, 0);
		gpio_free(pdata->gpio_ldo1_en);
	}

	snd_soc_unregister_card(card);

	tegra_asoc_utils_fini(&machine->util_data);

#ifdef CONFIG_SWITCH
	tegra_asoc_switch_unregister(&tegra_rt5640_headset_switch);
#endif
	kfree(machine);

	return 0;
}

static struct platform_driver tegra_rt5640_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
	},
	.probe = tegra_rt5640_driver_probe,
	.remove = __devexit_p(tegra_rt5640_driver_remove),
};

static int __init tegra_rt5640_modinit(void)
{
	return platform_driver_register(&tegra_rt5640_driver);
}
module_init(tegra_rt5640_modinit);

static void __exit tegra_rt5640_modexit(void)
{
	platform_driver_unregister(&tegra_rt5640_driver);
}
module_exit(tegra_rt5640_modexit);

MODULE_AUTHOR("Johnny Qiu <joqiu@nvidia.com>");
MODULE_DESCRIPTION("Tegra+RT5640 machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
