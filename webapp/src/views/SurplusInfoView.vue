<template>
    <BasePage
        :title="$t('surplusinfo.Information')"
        :isLoading="dataLoading"
        :show-reload="true"
        @reload="getSurplusInfo"
    >
        <CardElement :text="$t('surplusinfo.GeneralHeader')" textVariant="text-bg-primary" add-space table>
            <div class="card-body">
                <div class="table-responsive">
                    <table class="table table-striped table-hover">
                        <tbody>
                            <tr>
                                <th scope="row">{{ $t('surplusinfo.Status') }}</th>
                                <td class="value">
                                    <StatusBadge
                                        :status="SurplusStatus.enabled"
                                        true_text="surplusinfo.StatusEnabled"
                                        false_text="surplusinfo.StatusDisabled"
                                    />
                                </td>
                                <td></td>
                            </tr>
                            <template v-if="SurplusStatus.enabled">
                                <tr>
                                    <th scope="row">{{ $t('surplusinfo.RequirementsCheck') }}</th>
                                    <td class="value">
                                        {{ SurplusStatus.requirements_check }}
                                    </td>
                                    <td></td>
                                </tr>
                            </template>
                        </tbody>
                    </table>
                </div>
            </div>
        </CardElement>

        <template v-if="SurplusStatus.enabled">
            <CardElement :text="$t('surplusinfo.StateHeader')" textVariant="text-bg-primary" add-space table>
                <div class="card-body">
                    <div class="table-responsive">
                        <table class="table table-striped table-hover">
                            <tbody>
                                <tr>
                                    <th scope="row">{{ $t('surplusinfo.State') }}</th>
                                    <td class="value">
                                        {{ SurplusStatus.state }}
                                    </td>
                                    <td></td>
                                </tr>
                                <tr>
                                    <th scope="row">{{ $t('surplusinfo.Power') }}</th>
                                    <template v-if="SurplusStatus.surplus_power != -1">
                                        <td class="value">
                                            {{
                                                $n(SurplusStatus.surplus_power, 'decimal', {
                                                    minimumFractionDigits: 0,
                                                    maximumFractionDigits: 0,
                                                })
                                            }}
                                        </td>
                                    </template>
                                    <template v-else>
                                        <td class="value">---</td>
                                    </template>
                                    <td>W</td>
                                </tr>
                                <tr>
                                    <th scope="row">{{ $t('surplusinfo.MaxPower') }}</th>
                                    <template v-if="SurplusStatus.max_surplus_power != -1">
                                        <td class="value">
                                            {{
                                                $n(SurplusStatus.max_surplus_power, 'decimal', {
                                                    minimumFractionDigits: 0,
                                                    maximumFractionDigits: 0,
                                                })
                                            }}
                                        </td>
                                    </template>
                                    <template v-else>
                                        <td class="value">---</td>
                                    </template>
                                    <td>W</td>
                                </tr>
                            </tbody>
                        </table>
                    </div>
                </div>
            </CardElement>

            <CardElement :text="$t('surplusinfo.BulkHeader')" textVariant="text-bg-primary" add-space table>
                <div class="card-body">
                    <div class="table-responsive">
                        <table class="table table-striped table-hover">
                            <tbody>
                                <tr>
                                    <th scope="row">{{ $t('surplusinfo.BulkEnabled') }}</th>
                                    <td class="value">
                                        <StatusBadge
                                            :status="SurplusStatus.bulk_mode.enabled"
                                            true_text="surplusinfo.StatusEnabled"
                                            false_text="surplusinfo.StatusDisabled"
                                        />
                                    </td>
                                    <td></td>
                                </tr>
                                <template v-if="SurplusStatus.bulk_mode.enabled">
                                    <tr>
                                        <th scope="row">{{ $t('surplusinfo.BulkState') }}</th>
                                        <td class="value">
                                            {{ SurplusStatus.bulk_mode.state }}
                                        </td>
                                        <td></td>
                                    </tr>
                                    <template v-if="SurplusStatus.bulk_mode.slope_enabled">
                                        <tr>
                                            <th scope="row">{{ $t('surplusinfo.BulkSlopePower') }}</th>
                                            <template v-if="SurplusStatus.bulk_mode.slope_power != -1">
                                                <td class="value">
                                                    {{
                                                        $n(SurplusStatus.bulk_mode.slope_power, 'decimal', {
                                                            minimumFractionDigits: 0,
                                                            maximumFractionDigits: 0,
                                                        })
                                                    }}
                                                </td>
                                            </template>
                                            <template v-else>
                                                <td class="value">---</td>
                                            </template>
                                            <td>W</td>
                                        </tr>
                                        <tr>
                                            <th scope="row">{{ $t('surplusinfo.BulkMaxSlopePower') }}</th>
                                            <template v-if="SurplusStatus.bulk_mode.max_slope_power != -1">
                                                <td class="value">
                                                    {{
                                                        $n(SurplusStatus.bulk_mode.max_slope_power, 'decimal', {
                                                            minimumFractionDigits: 0,
                                                            maximumFractionDigits: 0,
                                                        })
                                                    }}
                                                </td>
                                            </template>
                                            <template v-else>
                                                <td class="value">---</td>
                                            </template>
                                            <td>W</td>
                                        </tr>
                                    </template>
                                    <tr>
                                        <th scope="row">{{ $t('surplusinfo.BulkBatteryReservePower') }}</th>
                                        <template v-if="SurplusStatus.bulk_mode.battery_reserve_power != -1">
                                            <td class="value">
                                                {{
                                                    $n(SurplusStatus.bulk_mode.battery_reserve_power, 'decimal', {
                                                        minimumFractionDigits: 0,
                                                        maximumFractionDigits: 0,
                                                    })
                                                }}
                                            </td>
                                        </template>
                                        <template v-else>
                                            <td class="value">---</td>
                                        </template>
                                        <td>W</td>
                                    </tr>
                                    <tr>
                                        <th scope="row">{{ $t('surplusinfo.Duration') }}</th>
                                        <td class="value">
                                            {{ SurplusStatus.bulk_mode.duration }}
                                        </td>
                                        <td></td>
                                    </tr>
                                </template>
                            </tbody>
                        </table>
                    </div>
                </div>
            </CardElement>

            <CardElement :text="$t('surplusinfo.AbsorptionHeader')" textVariant="text-bg-primary" add-space table>
                <div class="card-body">
                    <div class="table-responsive">
                        <table class="table table-striped table-hover">
                            <tbody>
                                <tr>
                                    <th scope="row">{{ $t('surplusinfo.AbsorptionEnabled') }}</th>
                                    <td class="value">
                                        <StatusBadge
                                            :status="SurplusStatus.absorption_mode.enabled"
                                            true_text="surplusinfo.StatusEnabled"
                                            false_text="surplusinfo.StatusDisabled"
                                        />
                                    </td>
                                    <td></td>
                                </tr>
                                <template v-if="SurplusStatus.absorption_mode.enabled">
                                    <tr>
                                        <th scope="row">{{ $t('surplusinfo.AbsorptionState') }}</th>
                                        <td class="value">
                                            {{ SurplusStatus.absorption_mode.state }}
                                        </td>
                                        <td></td>
                                    </tr>
                                    <tr>
                                        <th scope="row">{{ $t('surplusinfo.AbsorptionRegulationQuality') }}</th>
                                        <td class="value">
                                            {{ SurplusStatus.absorption_mode.regulation_quality }}
                                        </td>
                                        <td></td>
                                    </tr>
                                    <tr>
                                        <th scope="row">{{ $t('surplusinfo.Duration') }}</th>
                                        <td class="value">
                                            {{ SurplusStatus.absorption_mode.duration }}
                                        </td>
                                        <td></td>
                                    </tr>
                                </template>
                            </tbody>
                        </table>
                    </div>
                </div>
            </CardElement>
        </template>
    </BasePage>
</template>

<script lang="ts">
import BasePage from '@/components/BasePage.vue';
import CardElement from '@/components/CardElement.vue';
import StatusBadge from '@/components/StatusBadge.vue';
import type { SurplusStatus } from '@/types/SurplusStatus';
import { authHeader, handleResponse } from '@/utils/authentication';
import { defineComponent } from 'vue';

export default defineComponent({
    components: {
        BasePage,
        CardElement,
        StatusBadge,
    },
    data() {
        return {
            dataLoading: false,
            SurplusStatus: {} as SurplusStatus,
        };
    },
    created() {
        this.getSurplusInfo();
    },
    methods: {
        getSurplusInfo() {
            this.dataLoading = true;
            fetch('/api/surplus/status', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.SurplusStatus = data;
                })
                .catch(() => {
                    /* handleResponse already emits toast/redirect for HTTP errors;
                       this prevents an unhandled rejection for network failures */
                })
                .finally(() => {
                    this.dataLoading = false;
                });
        },
    },
});
</script>
