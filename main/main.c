 #include <errno.h>
 #include <dirent.h>
 #include <stdlib.h>
 #include "sdkconfig.h"
 #include "esp_console.h"
 #include "esp_check.h"
 #include "esp_partition.h"
 #include "driver/gpio.h"
 #include "tinyusb.h"
 #include "tusb_msc_storage.h"
 #include "esp_private/usb_phy.h"
 
 #if SOC_USB_SERIAL_JTAG_SUPPORTED
 #if !CONFIG_ESP_CONSOLE_SECONDARY_NONE
 #warning "A secondary serial console is not useful when using the console component. Please disable it in menuconfig."
 #endif
 #endif
 
 static const char *TAG = "example_main";
 static esp_console_repl_t *repl = NULL;
 
 
 
 #define BASE_PATH "/data" // base path to mount the partition
 
 
 
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
     ESP_LOGI(TAG, "Storage mounted to application: %s", event->mount_changed_data.is_mounted ? "Yes" : "No");
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

    static usb_phy_handle_t phy_hdl;
    static usb_phy_otg_io_conf_t otg_io_conf = USB_PHY_SELF_POWERED_DEVICE(6);
    // Configure USB PHY
    static usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_io_conf = &otg_io_conf,
    };
    usb_new_phy(&phy_conf, &phy_hdl);
}

static void tusb_device_task(void *arg)
{
    while (1) {
        tud_task();
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

    usb_phy_init();
    // tud_init(0);
    bool usb_init = tusb_init();
    if(usb_init == false){
        ESP_LOGE(TAG, "Failed to initialize USB stack.");
    } else {
        ESP_LOGI(TAG, "USB stack initialized.");
        xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 4096, NULL, 5, NULL, 0);
    }
 }
