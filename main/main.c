#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_console.h"
#include "esp_check.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h" // Note: This seems unused if only UAC is the goal.
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#include "esp_private/usb_phy.h"
#include "usb_device_uac.h"
#include "usb_descriptors.h"
#include "driver/i2s_std.h"
// #include "driver/i2c_master.h" // Note: This seems unused in the provided snippet.
#include "esp_log.h"
#include <inttypes.h> // For PRIu32

static const char *TAG = "example_main";

// --- Define I2S Configuration Constants ---
// These are examples, adjust them based on your hardware (ESP32 variant and I2S codec/DAC)
#define EXAMPLE_I2S_NUM         (I2S_NUM_0)
#define EXAMPLE_I2S_MCK_IO      (I2S_GPIO_UNUSED)  // Or I2S_GPIO_UNUSED if not used by your codec
#define EXAMPLE_I2S_BCK_IO      (GPIO_NUM_9)
#define EXAMPLE_I2S_WS_IO       (GPIO_NUM_45)
#define EXAMPLE_I2S_DO_IO       (GPIO_NUM_10)
#define EXAMPLE_I2S_DI_IO       (I2S_GPIO_UNUSED) // Not used for output only

#define EXAMPLE_AUDIO_SAMPLE_RATE (48000)
#define EXAMPLE_AUDIO_BIT_WIDTH   (I2S_DATA_BIT_WIDTH_16BIT) // Must match USB descriptor
#define EXAMPLE_I2S_DMA_DESC_NUM  (6)
#define EXAMPLE_I2S_DMA_FRAME_NUM (1248) // ~2ms buffer, stereo, 16-bit (adjust as needed)

// --- I2S Handles ---
static i2s_chan_handle_t i2s_tx_handle = NULL;
static i2s_chan_handle_t i2s_rx_handle = NULL; // Keep if you plan to use RX, otherwise can remove its setup

static void usb_phy_init(void)
{
    const gpio_config_t vbus_gpio_config = {
        .pin_bit_mask = BIT64(GPIO_NUM_6),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&vbus_gpio_config));

    static usb_phy_handle_t phy_hdl; // Make static to ensure it remains valid
    // The otg_io_conf struct must also be static or global if phy_conf is static/global
    // and its address is stored. For local usage like this, it's fine.
    usb_phy_otg_io_conf_t otg_io_conf = USB_PHY_SELF_POWERED_DEVICE(GPIO_NUM_6); // Use the defined GPIO_NUM_6

    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_io_conf = &otg_io_conf, // Pass address of local copy
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &phy_hdl));
}

#define BASE_PATH "/data" // base path to mount the partition
static uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
 
 
 
// mount the partition and show all the files in BASE_PATH
static void _mount(void)
{
    ESP_LOGI(TAG, "Mount storage...");
    ESP_ERROR_CHECK(tinyusb_msc_storage_mount(BASE_PATH));

    // List all the files in this directory
    ESP_LOGI(TAG, "\nls command output:");
    struct dirent *d;
    DIR *dh = opendir(BASE_PATH);
    if (!dh) {
        if (errno == ENOENT) {
            //If the directory is not found
            ESP_LOGE(TAG, "Directory doesn't exist %s", BASE_PATH);
        } else {
            //If the directory is not readable then throw error and exit
            ESP_LOGE(TAG, "Unable to read directory %s", BASE_PATH);
        }
        return;
    }
    //While the next entry is not readable we will print directory files
    while ((d = readdir(dh)) != NULL) {
        printf("%s\n", d->d_name);
    }
    return;
}



// callback that is delivered when storage is mounted/unmounted by application.
static void storage_mount_changed_cb(tinyusb_msc_event_t *event)
{
    static bool inited = false;
    ESP_LOGI("USB", "Storage mounted to application: %s", event->mount_changed_data.is_mounted ? "Yes" : "No");
    if(event->mount_changed_data.is_mounted){
        if(!inited){
            return;
        }
        esp_tusb_deinit_console(TINYUSB_CDC_ACM_0);
        inited = false;
    } else {
        if(inited){
            return;
        }
        esp_tusb_init_console(TINYUSB_CDC_ACM_0);
        inited = true;
    }
}

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    ESP_LOGI(TAG, "Initializing wear levelling");

    const esp_partition_t *data_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition. Check the partition table.");
        return ESP_ERR_NOT_FOUND;
    }

    return wl_mount(data_partition, wl_handle);
}

static void tusb_device_task(void *arg)
{
    while (1) {
        tud_task();
        // Add a small delay to yield to other tasks if tud_task() is non-blocking
        // vTaskDelay(pdMS_TO_TICKS(1)); // Example, adjust as needed
    }
}

// This is the UAC callback where USB audio data is received
static esp_err_t uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_written = 0;

    if (i2s_tx_handle == NULL) {
        ESP_LOGE(TAG, "I2S TX Handle not initialized!");
        return ESP_ERR_INVALID_STATE;
    }

    // Write the received audio data to I2S
    // The timeout portMAX_DELAY means it will block indefinitely until space is available.
    // You might want to use a smaller timeout depending on your application's needs.
    ret = i2s_channel_write(i2s_tx_handle, buf, len, &bytes_written, portMAX_DELAY);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
    } else if (bytes_written < len) {
        ESP_LOGW(TAG, "I2S write underrun: wrote %d of %d bytes", bytes_written, len);
        // This might happen if the I2S bus is slower than the USB audio stream
        // or if the I2S DMA buffer is too small.
    }
    return ret;
}

static void i2s_driver_init(void)
{
    // I2S channel configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    chan_cfg.dma_desc_num = EXAMPLE_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = EXAMPLE_I2S_DMA_FRAME_NUM; // Must be <= dma_desc_num
                                                       // Frame num represents samples PER CHANNEL.
                                                       // e.g., if 16-bit stereo, frame_num=240 means 240*2(ch)*2(bytes)=960 bytes
    // ESP_LOGI(TAG, "I2S DMA Buffer size: %d frames, %d descriptors. Total bytes: %u",
    //          chan_cfg.dma_frame_num, chan_cfg.dma_desc_num,
    //          chan_cfg.dma_frame_num * (EXAMPLE_AUDIO_BIT_WIDTH / 8) * 2 * chan_cfg.dma_desc_num); // Assuming stereo

    // Allocate TX and RX channels. If only TX is needed, you can skip RX.
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_handle, NULL)); // Only TX for output
    // If you also need RX:
    // ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_handle, &i2s_rx_handle));


    // I2S standard mode configuration
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(EXAMPLE_AUDIO_BIT_WIDTH, I2S_SLOT_MODE_MONO), // Assuming stereo output
        .gpio_cfg = {
            .mclk = EXAMPLE_I2S_MCK_IO,
            .bclk = EXAMPLE_I2S_BCK_IO,
            .ws = EXAMPLE_I2S_WS_IO,
            .dout = EXAMPLE_I2S_DO_IO,
            .din = EXAMPLE_I2S_DI_IO, // Set to I2S_GPIO_UNUSED if not used
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };


    // Initialize TX channel
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg));
    ESP_LOGI(TAG, "I2S TX channel initialized.");

    // Initialize RX channel if allocated
    // if (i2s_rx_handle) {
    //     ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_handle, &std_cfg));
    //     ESP_LOGI(TAG, "I2S RX channel initialized.");
    // }

    // Enable TX channel
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_handle));
    ESP_LOGI(TAG, "I2S TX channel enabled.");

    // Enable RX channel if allocated and initialized
    // if (i2s_rx_handle) {
    //     ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_handle));
    //     ESP_LOGI(TAG, "I2S RX channel enabled.");
    // }

    /* Get the initial MCLK */
    // This part is mostly for debugging or if you need to know the exact MCLK.
    // i2s_std_clk_config_t clk_cfg_tmp; // Temporary to get current clock config
    // ESP_ERROR_CHECK(i2s_channel_get_std_clk_config(i2s_tx_handle, &clk_cfg_tmp));
    // init_mclk_freq_hz = clk_cfg_tmp.mclk_hz;
    // ESP_LOGI(TAG, "Initial MCLK: %" PRIu32 " Hz", init_mclk_freq_hz);
    // ESP_LOGI(TAG, "Configured Sample Rate: %d Hz, Bit Width: %d bits",
    //          EXAMPLE_AUDIO_SAMPLE_RATE, EXAMPLE_AUDIO_BIT_WIDTH);

    // You might want to pre-fill the DMA buffer with silence if there's a delay before USB audio starts
    // This helps avoid initial glitches on some DACs.
    // size_t written_silence;
    // uint8_t silence_buf[1024] = {0}; // Adjust size as needed
    // i2s_channel_write(i2s_tx_handle, silence_buf, sizeof(silence_buf), &written_silence, portMAX_DELAY);

    const gpio_config_t amp_mode_config = {
        .pin_bit_mask = BIT64(GPIO_NUM_8),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&amp_mode_config));
    gpio_set_level(GPIO_NUM_8, 1);

}

void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    /* initialization */
    size_t rx_size = 0;

    /* read */
    esp_err_t ret = tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret == ESP_OK) {
        // Ensure received data is null-terminated and fits in the buffer
        static char line[255]; // Console input buffer
        size_t len = MIN(sizeof(line) - 1, rx_size); // Leave room for null terminator
        snprintf(line, len + 1, "%.*s", (int)len, buf); // Null-terminate and truncate if necessary

        // Pass the line to the console
        int console_ret;
        esp_console_run(line, &console_ret);
        ESP_LOGI("USB", "Running %s.", line);
    } else {
        ESP_LOGE("USB", "Read error");
    }
}

static bool cdc_port_open = false;

void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    bool dtr = event->line_state_changed_data.dtr;

    // als hostpoort nét opengezet is → onthoud dat we in “open” zijn
    if (dtr && !cdc_port_open) {
        ESP_LOGI("USB", "CDC DTR gone up → host-poort open");
        cdc_port_open = true;
    }
    // als hostpoort wél eerder open was en nu dichtgaat → unmount
    else if (!dtr && cdc_port_open) {
        ESP_LOGI("USB", "CDC DTR gone down → unmount MSC");
        tinyusb_msc_storage_unmount();
        cdc_port_open = false;
        // (optioneel) na korte vertraging weer remounten:
        // vTaskDelay(pdMS_TO_TICKS(100));
        // tinyusb_msc_storage_mount(BASE_PATH);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing storage...");
 
     static wl_handle_t wl_handle = WL_INVALID_HANDLE;
     ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));
 
     const tinyusb_msc_spiflash_config_t config_spi = {
         .wl_handle = wl_handle,
         .callback_mount_changed = storage_mount_changed_cb,  /* First way to register the callback. This is while initializing the storage. */
         .mount_config.max_files = 5,
     };
     ESP_ERROR_CHECK(tinyusb_msc_storage_init_spiflash(&config_spi));
     ESP_ERROR_CHECK(tinyusb_msc_register_callback(TINYUSB_MSC_EVENT_MOUNT_CHANGED, storage_mount_changed_cb)); /* Other way to register the callback i.e. registering using separate API. If the callback had been already registered, it will be overwritten. */
 
     //mounted in the app by default
     _mount();
 
     ESP_LOGI(TAG, "USB MSC initialization");

     tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx = &tinyusb_cdc_rx_callback, // the first way to register a callback
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = &tinyusb_cdc_line_state_changed_callback,
        .callback_line_coding_changed = NULL
    };

    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));

    ESP_LOGI(TAG, "App main started");

    // 1. Initialize I2S driver first
    i2s_driver_init();
    ESP_LOGI(TAG, "I2S driver initialized.");

    // 2. Configure UAC device
    uac_device_config_t uac_config = {
        .skip_tinyusb_init = true,     // We will init TinyUSB manually
        .output_cb = uac_device_output_cb,
        .cb_ctx = NULL,
        .spk_itf_num = ITF_NUM_AUDIO_STREAMING_SPK, // Ensure this matches your USB descriptor
        // Add .mic_itf_num if you also have microphone input
    };
    // Initialize UAC. This sets up descriptor parts related to UAC.
    uac_device_init(&uac_config);
    ESP_LOGI(TAG, "UAC device initialized.");

    // 3. Initialize USB PHY
    usb_phy_init();
    ESP_LOGI(TAG, "USB PHY initialized.");

    // 4. Initialize TinyUSB stack
    bool usb_init_stat = tusb_init();
    if (usb_init_stat == false) {
        ESP_LOGE(TAG, "Failed to initialize TinyUSB stack.");
        return; // Or handle error appropriately
    }
    ESP_LOGI(TAG, "TinyUSB stack initialized.");

    // 5. Create TinyUSB device task
    // It's good practice to check the return value of xTaskCreatePinnedToCore
    BaseType_t task_created = xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 4096, NULL, 5, NULL, 0);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TinyUSB task.");
        return; // Or handle error appropriately
    }
    ESP_LOGI(TAG, "TinyUSB task created and pinned to core 0.");

    ESP_LOGI(TAG, "Setup complete. Waiting for USB connection and audio data...");
}