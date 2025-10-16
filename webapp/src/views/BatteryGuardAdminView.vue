<template>
    <BasePage :title="$t('batteryguardadmin.BatteryGuardSettings')" :isLoading="dataLoading">
        <BootstrapAlert v-model="showAlert" dismissible :variant="alertType">
            {{ alertMessage }}
        </BootstrapAlert>

        <BootstrapAlert v-model="configAlert" variant="warning">
            {{ $t('batteryguardadmin.ConfigAlertMessage') }}
        </BootstrapAlert>

        <CardElement
            :text="$t('batteryguardadmin.ConfigHints')"
            textVariant="text-bg-primary"
            v-if="getConfigHints().length"
        >
            <div class="row">
                <div class="col-sm-12">
                    {{ $t('batteryguardadmin.ConfigHintsIntro') }}
                    <ul class="mb-0">
                        <li v-for="(hint, idx) in getConfigHints()" :key="idx">
                            <b v-if="hint.severity === 'requirement'"
                                >{{ $t('batteryguardadmin.ConfigHintRequirement') }}:</b
                            >
                            <b v-if="hint.severity === 'optional'">{{ $t('batteryguardadmin.ConfigHintOptional') }}:</b>
                            {{ $t('batteryguardadmin.ConfigHint' + hint.subject) }}
                        </li>
                    </ul>
                </div>
            </div>
        </CardElement>

        <form @submit="saveConfig" v-if="!configAlert">
            <CardElement :text="$t('batteryguardadmin.BatteryGuardConfiguration')" textVariant="text-bg-primary">
                <InputElement
                    :label="$t('batteryguardadmin.EnableBatteryGuard')"
                    v-model="batteryGuardConfig.enabled"
                    type="checkbox"
                    wide
                />

                <div
                    v-if="!batteryGuardConfig.enabled"
                    class="alert alert-secondary"
                    role="alert"
                    v-html="$t('batteryguardadmin.UseInfo')"
                ></div>
            </CardElement>

            <template v-if="batteryGuardConfig.enabled">
                <CardElement
                    :text="$t('batteryguardadmin.BatteryGuardLimiter')"
                    textVariant="text-bg-primary"
                    add-space
                >
                    <InputElement
                        v-on:change="checkOptionLimiter"
                        :label="$t('batteryguardadmin.CompensationEnabled')"
                        v-model="batteryGuardConfig.voltage_drop_compensation_enabled"
                        type="checkbox"
                        wide
                    />

                    <template v-if="batteryGuardConfig.voltage_drop_compensation_enabled">
                        <InputElement
                            :label="$t('batteryguardadmin.LowVoltageLimiterEnabled')"
                            v-model="batteryGuardConfig.low_voltage_limiter_enabled"
                            type="checkbox"
                            :tooltip="$t('batteryguardadmin.LowVoltageLimiterHint')"
                            wide
                            :disabled="
                                batteryGuardConfig.recharge_helper_enabled && !batteryGuardConfig.use_voltage_thresholds
                            "
                        />

                        <template v-if="batteryGuardConfig.low_voltage_limiter_enabled">
                            <InputElement
                                :label="$t('batteryguardadmin.DPLStopThreshold')"
                                v-model="batteryGuardConfig.dpl_stop_threshold"
                                type="number"
                                min="0"
                                max="100"
                                step="1"
                                postfix="V"
                                :tooltip="$t('batteryguardadmin.DPLStopThresholdHint')"
                                wide
                                :disabled="true"
                            />
                        </template>

                        <InputElement
                            :label="$t('batteryguardadmin.InternalResistance')"
                            v-model="batteryGuardConfig.internal_resistance"
                            type="number"
                            min="0"
                            max="50"
                            step="0.1"
                            postfix="mOhm"
                            :tooltip="$t('batteryguardadmin.InternalResistanceHint')"
                            wide
                        />
                    </template>

                    <div
                        v-if="!batteryGuardConfig.voltage_drop_compensation_enabled"
                        class="alert alert-secondary"
                        role="alert"
                        v-html="$t('batteryguardadmin.LimitInfo')"
                    ></div>
                </CardElement>
            </template>

            <template v-if="batteryGuardConfig.enabled">
                <CardElement
                    :text="$t('batteryguardadmin.RechargeConfiguration')"
                    textVariant="text-bg-primary"
                    add-space
                >
                    <InputElement
                        v-on:change="checkOptionThresholds"
                        :label="$t('batteryguardadmin.RechargeHelperEnabled')"
                        v-model="batteryGuardConfig.recharge_helper_enabled"
                        type="checkbox"
                        wide
                    />

                    <template v-if="batteryGuardConfig.recharge_helper_enabled">
                        <InputElement
                            :label="$t('batteryguardadmin.RechargeHelperExcessiveDisabled')"
                            v-model="batteryGuardConfig.excessive_solar_power_disabled"
                            type="checkbox"
                            :tooltip="$t('batteryguardadmin.RechargeHelperExcessiveHint')"
                            wide
                        />

                        <InputElement
                            :label="$t('batteryguardadmin.RechargeHelperUseVoltage')"
                            v-model="batteryGuardConfig.use_voltage_thresholds"
                            type="checkbox"
                            :tooltip="$t('batteryguardadmin.RechargeHelperUseVoltageHint')"
                            wide
                            :disabled="
                                batteryGuardConfig.voltage_drop_compensation_enabled &&
                                batteryGuardConfig.low_voltage_limiter_enabled
                            "
                        />

                        <InputElement
                            :label="$t('batteryguardadmin.RechargeIdle')"
                            v-model="batteryGuardConfig.duration_idle"
                            type="number"
                            min="0"
                            max="60"
                            step="1"
                            :postfix="$t('batteryguardadmin.Days')"
                            :tooltip="$t('batteryguardadmin.RechargeIdleHint')"
                            wide
                        />

                        <InputElement
                            :label="$t('batteryguardadmin.RechargeStage1')"
                            v-model="batteryGuardConfig.duration_stage1"
                            type="number"
                            min="0"
                            max="30"
                            step="1"
                            :postfix="$t('batteryguardadmin.Days')"
                            :tooltip="$t('batteryguardadmin.RechargeStage1Hint')"
                            wide
                        />

                        <InputElement
                            :label="$t('batteryguardadmin.RechargeStage2')"
                            v-model="batteryGuardConfig.duration_stage2"
                            type="number"
                            min="0"
                            max="30"
                            step="1"
                            :postfix="$t('batteryguardadmin.Days')"
                            :tooltip="$t('batteryguardadmin.RechargeStage2Hint')"
                            wide
                        />

                        <template v-if="batteryGuardConfig.use_voltage_thresholds">
                            <InputElement
                                :label="$t('batteryguardadmin.RechargeMaxStartThreshold')"
                                v-model="batteryGuardConfig.max_start_threshold"
                                type="number"
                                min="0"
                                max="100"
                                step="0.1"
                                postfix="V"
                                :tooltip="$t('batteryguardadmin.RechargeMaxStartHint')"
                                wide
                            />

                            <InputElement
                                :label="$t('batteryguardadmin.RechargeMaxStopThreshold')"
                                v-model="batteryGuardConfig.max_stop_threshold"
                                type="number"
                                min="0"
                                max="100"
                                step="0.1"
                                postfix="V"
                                :tooltip="$t('batteryguardadmin.RechargeMaxStopHint')"
                                wide
                            />
                        </template>

                        <template v-else>
                            <InputElement
                                :label="$t('batteryguardadmin.RechargeMaxStartThreshold')"
                                v-model="batteryGuardConfig.max_soc_start_threshold"
                                type="number"
                                min="0"
                                max="100"
                                step="1"
                                postfix="%"
                                :tooltip="$t('batteryguardadmin.RechargeMaxStartHint')"
                                wide
                            />

                            <InputElement
                                :label="$t('batteryguardadmin.RechargeMaxStopThreshold')"
                                v-model="batteryGuardConfig.max_soc_stop_threshold"
                                type="number"
                                min="0"
                                max="100"
                                step="1"
                                postfix="%"
                                :tooltip="$t('batteryguardadmin.RechargeMaxStopHint')"
                                wide
                            />
                        </template>

                        <InputElement
                            :label="$t('batteryguardadmin.RechargePowerLimit')"
                            v-model="batteryGuardConfig.upper_power_limit"
                            type="number"
                            min="0"
                            max="2000"
                            step="1"
                            postfix="W"
                            :tooltip="$t('batteryguardadmin.RechargePowerLimitHint')"
                            wide
                        />
                    </template>

                    <div
                        v-if="!batteryGuardConfig.recharge_helper_enabled"
                        class="alert alert-secondary"
                        role="alert"
                        v-html="$t('batteryguardadmin.RechargeInfo')"
                    ></div>
                </CardElement>
            </template>

            <FormFooter @reload="getMetaData" />
        </form>
    </BasePage>
</template>

<script lang="ts">
import BasePage from '@/components/BasePage.vue';
import BootstrapAlert from '@/components/BootstrapAlert.vue';
import CardElement from '@/components/CardElement.vue';
import FormFooter from '@/components/FormFooter.vue';
import InputElement from '@/components/InputElement.vue';
import { authHeader, handleResponse } from '@/utils/authentication';
import { defineComponent } from 'vue';
import type { BatteryGuardConfig, BatteryGuardMetaData } from '@/types/BatteryGuardConfig';

export default defineComponent({
    components: {
        BasePage,
        BootstrapAlert,
        CardElement,
        FormFooter,
        InputElement,
    },
    data() {
        return {
            dataLoading: false,
            batteryGuardConfig: {} as BatteryGuardConfig,
            batteryGuardMetaData: {} as BatteryGuardMetaData,
            alertMessage: '',
            alertType: 'info',
            showAlert: false,
            configAlert: false,
        };
    },
    created() {
        this.getMetaData();
    },
    methods: {
        getConfigHints(): { severity: string; subject: string }[] {
            const meta = this.batteryGuardMetaData;
            const hints = [];

            if (meta.battery_enabled !== true) {
                hints.push({ severity: 'requirement', subject: 'BatteryRequired' });
                this.configAlert = true;
            }

            return hints;
        },
        getMetaData() {
            this.dataLoading = true;
            fetch('/api/batteryguard/metadata', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.batteryGuardMetaData = data;
                    this.getConfigData();
                })
                .catch((error) => {
                    this.alertMessage = error.message || 'Failed to load metadata';
                    this.alertType = 'danger';
                    this.showAlert = true;
                    this.dataLoading = false;
                });
        },
        getConfigData() {
            fetch('/api/batteryguard/config', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.batteryGuardConfig = data;
                    this.dataLoading = false;
                })
                .catch((error) => {
                    this.alertMessage = error.message || 'Failed to load configuration';
                    this.alertType = 'danger';
                    this.showAlert = true;
                    this.dataLoading = false;
                });
        },
        saveConfig(e: Event) {
            e.preventDefault();

            const formData = new FormData();
            formData.append('data', JSON.stringify(this.batteryGuardConfig));

            fetch('/api/batteryguard/config', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((response) => {
                    this.alertMessage = response.message;
                    this.alertType = response.type;
                    this.showAlert = true;
                    window.scrollTo(0, 0);
                })
                .catch((error) => {
                    this.alertMessage = error.message || 'Failed to save configuration';
                    this.alertType = 'danger';
                    this.showAlert = true;
                    window.scrollTo(0, 0);
                });
        },
        checkOptionLimiter(): void {
            if (this.batteryGuardConfig.recharge_helper_enabled && !this.batteryGuardConfig.use_voltage_thresholds) {
                this.batteryGuardConfig.low_voltage_limiter_enabled = false;
            }
        },
        checkOptionThresholds(): void {
            if (
                this.batteryGuardConfig.voltage_drop_compensation_enabled &&
                this.batteryGuardConfig.low_voltage_limiter_enabled
            ) {
                this.batteryGuardConfig.use_voltage_thresholds = true;
            }
        },
    },
});
</script>
