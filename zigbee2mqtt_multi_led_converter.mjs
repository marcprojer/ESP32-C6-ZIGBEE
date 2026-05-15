import {light} from "zigbee-herdsman-converters/lib/modernExtend";

/** @type {import('zigbee-herdsman-converters/lib/types').DefinitionWithExtend} */
export default {
    zigbeeModel: ["ZBMultiLED_12V"],
    model: "ESP32-C6-3CH-LED",
    vendor: "DIY",
    description: "ESP32-C6 Zigbee controller with three independent LED outputs",
    extend: [
        light({endpoint: "l1"}),
        light({endpoint: "l2"}),
        light({endpoint: "l3"}),
    ],
    endpoint: () => ({
        l1: 10,
        l2: 11,
        l3: 12,
    }),
};
