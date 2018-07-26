#include "SlaveSPI.h"

int         SlaveSPI::vector_size    = 0;
SlaveSPI ** SlaveSPI::SlaveSPIVector = NULL;

void call_matcher_after_queueing(spi_slave_transaction_t * trans) {  // Call the hook matched to its transaction.
    for (int i = 0; i < SlaveSPI::vector_size; i++)
        if (SlaveSPI::SlaveSPIVector[i]->match(trans)) SlaveSPI::SlaveSPIVector[i]->callbackAfterQueueing(trans);
}

void call_matcher_after_transmission(spi_slave_transaction_t * trans) {  // Call the hook matched to its transaction.
    for (int i = 0; i < SlaveSPI::vector_size; i++)
        if (SlaveSPI::SlaveSPIVector[i]->match(trans)) SlaveSPI::SlaveSPIVector[i]->callbackAfterTransmission(trans);
}

SlaveSPI::SlaveSPI(spi_host_device_t spi_host) {
    this->spi_host = spi_host;

    SlaveSPI ** temp = new SlaveSPI *[vector_size + 1];  // Create a new instance array
    for (int i = 0; i < vector_size; i++) {              // Relocate all instances into the new array
        temp[i] = SlaveSPIVector[i];
    }
    temp[vector_size++] = this;  // Put this instance into
    delete[] SlaveSPIVector;     // Delete the old one
    SlaveSPIVector = temp;       // Point to the new one

    input_stream  = "";
    output_stream = "";
}

void SlaveSPI::begin(gpio_num_t so, gpio_num_t si, gpio_num_t sclk, gpio_num_t ss, size_t buffer_size, int (*callback)()) {
    callback_after_transmission = callback;

    max_buffer_size = buffer_size;  // should set to the minimum transaction length
    tx_buffer       = (byte *)heap_caps_malloc(max(max_buffer_size, 32), SPI_MALLOC_CAP);
    rx_buffer       = (byte *)heap_caps_malloc(max(max_buffer_size, 32), SPI_MALLOC_CAP);
    memset(tx_buffer, 0, max_buffer_size);
    memset(rx_buffer, 0, max_buffer_size);

    /*
    Initialize the Slave-SPI module:
    */
    spi_bus_config_t buscfg = {.mosi_io_num = si, .miso_io_num = so, .sclk_io_num = sclk};
    gpio_set_pull_mode(sclk, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(ss,   GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(si,   GPIO_PULLUP_ONLY);

    spi_slave_interface_config_t slvcfg = {
        .spics_io_num = ss,              //< CS GPIO pin for this device
        .flags        = 0,               //< Bitwise OR of SPI_SLAVE_* flags
        .queue_size   = SPI_QUEUE_SIZE,  //< Transaction queue size.
                                         //  This sets how many transactions can be 'in the air' (
                                         //    queued using spi_slave_queue_trans -- non-blocking --
                                         //    but not yet finished using spi_slave_get_trans_result -- blocking)
                                         //    at the same time
        .mode          = SPI_MODE,       //< SPI mode (0-3)
        .post_setup_cb = call_matcher_after_queueing,     //< Called after the SPI registers are loaded with new data
        .post_trans_cb = call_matcher_after_transmission  //< Called after a transaction is done
    };

    if (esp_err_t err = spi_slave_initialize(spi_host, &buscfg, &slvcfg, SPI_DMA)) {  // Setup the SPI module
        Serial.print(F("[SlaveSPI::begin] spi_slave_initialize err: "));
        Serial.println(err);
    }

    /*
    Prepare transaction queue:

    The amount of data written to the buffers is limited by the __length__ member of the transaction structure: 
        the driver will never read/write more data than indicated there. 
    The __length__ cannot define the actual length of the SPI transaction, 
        this is determined by the master as it drives the clock and CS lines. 
    
    The actual length transferred can be read from the __trans_len__ member 
        of the spi_slave_transaction_t structure after transaction. 
    
    In case the length of the transmission is larger than the buffer length,
        only the start of the transmission will be sent and received, 
        and the __trans_len__ is set to __length__ instead of the actual length. 
    It's recommended to set __length__ longer than the maximum length expected if the __trans_len__ is required. 
    
    In case the transmission length is shorter than the buffer length, only data up to the length of the buffer will be exchanged.
    */
    transaction = new spi_slave_transaction_t{
        .length    = max_buffer_size * 8,  //< Total data length, in bits -- maximum receivable
        .trans_len = 0,                    //< Transaction data length, in bits -- actual received
        .tx_buffer = tx_buffer,            //< tx buffer, or NULL for no MOSI phase
        .rx_buffer = rx_buffer,            //< rx buffer, or NULL for no MISO phase
        .user      = NULL                  //< User-defined variable. Can be used to store e.g. transaction ID.
    };

    initTransmissionQueue();
}

void SlaveSPI::callbackAfterQueueing(spi_slave_transaction_t * trans) {  // called when the trans is set in the queue
    // TODO: data ready -- trig hand-check pin
}

void SlaveSPI::callbackAfterTransmission(spi_slave_transaction_t * trans) {  // called when the trans has finished
    for (int i = 0; i < max_buffer_size; i++) {
        input_stream += ((char *)transaction->rx_buffer)[i];  // Aggregate
        ((char *)transaction->rx_buffer)[i] = (char)0;        // Clean
    }
    initTransmissionQueue();  // Re-initialize

    int ret = callback_after_transmission();
}

void SlaveSPI::initTransmissionQueue() {
    // Prepare out-going data for next master request
    int i = 0;
    for (; i < max_buffer_size && i < output_stream.length(); i++) {  // NOT over buffer size
        ((char *)transaction->tx_buffer)[i] = output_stream[i];       // Copy prepared data to out-going queue
    }
    output_stream = &(output_stream[i]);  // Segmentation. The remain is left for future.

    // Setup for receiving buffer. Prepare for next transaction
    transaction->length    = max_buffer_size * 8;
    transaction->trans_len = 0;     // Set zero on slave's actual received data.
    transaction->user      = NULL;  // XXX: reset?

    if (esp_err_t err = spi_slave_queue_trans(spi_host, transaction, portMAX_DELAY)) {  // Queue. Ready for sending if receiving
        Serial.print(F("[SlaveSPI::begin] spi_slave_queue_trans err: "));
        Serial.println(err);
    }
}

void SlaveSPI::write(String & msg) {  // used to queue data to transmit
    for (int i = 0; i < msg.length(); i++) {
        output_stream += msg[i];
    }
}

String SlaveSPI::read() {
    String tmp = input_stream;
    input_stream = "";
    return tmp;
}

byte SlaveSPI::readByte() {
    byte tmp = input_stream[0];
    input_stream.remove(0, 1);
    return tmp;
}
