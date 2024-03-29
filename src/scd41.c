#include <scd41.h>

#include <esp_log.h>
#include <esp_err.h>

#define SCD41_CMD_SERIALNUM 0x36, 0x82			 // 0x3682
#define SCD41_CMD_SET_ASC_EN 0x24, 0x16			 // 0x2416
#define SCD41_CMD_GET_ASC_EN 0x23, 0x13			 // 0x2313
#define SCD41_CMD_READMEASURE 0xec, 0x05		 // 0xec05
#define SCD41_CMD_SINGLESHOT 0x21, 0x9d			 // 0x219d
#define SCD41_CMD_LOWPOWER_PERIODIC 0x21, 0xac	 // 0x21b1
#define SCD41_SELFTEST 0x36, 0x39				 // 0x3639
#define SCD41_CMD_FORCE_RECALIBRATION 0x36, 0x2f // 0x362f

#define SCD41_CMD_GET_TEMP_OFF 0x23, 0x18 // 0x2318

// CRC defines
#define CRC8_POLYNOMIAL 0x31
#define CRC8_INIT 0xFF

/**
 * recalibrate to 415 ppm = 0x019F; 
 * sources:
 * recent data on https://scrippsco2.ucsd.edu/
 * recent data via https://atmosphere.copernicus.eu/charts/cams/carbon-dioxide-forecasts
 * viz. https://atmosphere.copernicus.eu/charts/cams/carbon-dioxide-forecasts?facets=undefined&time=2022092600,60,2022092812&projection=classical_central_europe&layer_name=composition_co2_totalcolumn
 */
#define SCD41_CO2_RECALIBRATION_VAL 0x01, 0x9F

void co2_init(uint8_t address)
{
	i2c_hal_init_port_1();

	if (co2_disable_asc(address))
		ESP_LOGD("CO2_INIT", "ASC enabled");
	else
		ESP_LOGD("CO2_INIT", "ASC disabled");
}

/**
 * CRC8 code taken from Sensirion SCD41 Datasheet: https://nl.mouser.com/datasheet/2/682/Sensirion_CO2_Sensors_SCD4x_Datasheet-2321195.pdf
 * @brief calculate the CRC for an SCD41 I2C message
 *
 * @param data pointer to the received i2c data
 * @param count size of the buffer
 *
 * @return calculated CRC8
 */
uint8_t scd41_crc8(const uint8_t *data, uint16_t count)
{

	uint16_t current_byte;
	uint8_t crc = CRC8_INIT;
	uint8_t crc_bit;

	for (current_byte = 0; current_byte < count; ++current_byte)
	{
		crc ^= (data[current_byte]);
		for (crc_bit = 8; crc_bit > 0; --crc_bit)
		{
			if (crc & 0x80)
				crc = (crc << 1) ^ CRC8_POLYNOMIAL;
			else
				crc = (crc << 1);
		}
	}

	return crc;
}

/**
 * @brief read the serial number of the CO2 sensor
 *
 * @param address i2c address of the device
 *
 * @return the serial number
 */
uint64_t co2_get_serial(uint8_t address)
{
	uint8_t cmd[2] = {SCD41_CMD_SERIALNUM};
	// Write the get-serial command, do not stop the i2c communication:
	twomes_i2c_write_port_1(address, &cmd[0], sizeof(cmd), I2C_SEND_NO_STOP);

	// Wait for 1ms, SCD41 processing time
	vTaskDelay(SCD41_WAIT_MS / portTICK_PERIOD_MS);

	// Issue a read command:
	uint8_t serialNumber[9]; // 3 16 bit numbers and 3 8-bit CRCs
	twomes_i2c_read_port_1(address, &serialNumber[0], sizeof(serialNumber));

	// For debug: log the serial numbers, the received checksums and the calculated checksums:
	uint8_t crc1, crc2, crc3; // SCD41 sends a crc after every word (16 bits)
	crc1 = scd41_crc8(&serialNumber[0], 2);
	crc2 = scd41_crc8(&serialNumber[3], 2);
	crc3 = scd41_crc8(&serialNumber[6], 2);

	ESP_LOGD("SERIAL_CRC", "\n\n Received Serial number %4X %4X %4X \n With CRC received:calculated \n %2x : %2x \n %2x : %2x \n %2x : %2x \n", serialNumber[0], serialNumber[3], serialNumber[6], serialNumber[2], crc1, serialNumber[5], crc2, serialNumber[8], crc3);
	return 0;
}
/**
 * @brief disable the SCD41 ASC (Automatic Self Calibration)
 * Device performs a read immeadiately after to check for disable success
 * @param address i2c address of the device
 *
 * @return value of ASC after write (0 == success)
 */
uint8_t co2_disable_asc(uint8_t address)
{
	// generate command buffer:
	uint8_t x[2] = {0, 0};
	uint8_t disable_asc_cmd[5] = {SCD41_CMD_SET_ASC_EN, 0, 0, scd41_crc8(x, 2)}; // Command buffer with generated CRC, write 0x0000 to SET_ASC address
	// Write to I2C:
	twomes_i2c_write_port_1(address, disable_asc_cmd, sizeof disable_asc_cmd, I2C_SEND_STOP);

	vTaskDelay(SCD41_WAIT_MS / portTICK_PERIOD_MS); // Give SCD41 time for processing
	// Perform read to check if disabling succeeded:
	uint8_t check_asc_cmd[2] = {SCD41_CMD_GET_ASC_EN};
	twomes_i2c_write_port_1(address, check_asc_cmd, sizeof check_asc_cmd, I2C_SEND_NO_STOP);
	vTaskDelay(SCD41_WAIT_MS / portTICK_PERIOD_MS); // Give SCD41 time for processing
	// Read the result:
	uint8_t response_buffer[2];
	twomes_i2c_read_port_1(address, response_buffer, sizeof response_buffer);

	// Debug print:
	ESP_LOGD("ASC", "Received Response: %2X, %2X", response_buffer[0], response_buffer[1]);

	return response_buffer[1];
}

/**
 * @brief Recalibrate the CO2 sensor.
 * Device performs a read immeadiately after to check for recalibration success
 * @param address i2c address of the device
 *
 * @return
 * 	ESP_OK: success
 * 	ESP_ERR_INVALID_RESPONSE: failed
 */
esp_err_t co2_force_recalibration(uint8_t address, int16_t* offset)
{
	// First gather measurements for more than 3 minutes. This is needed before calibration.
	// 3 minutes is 3 * 60 = 180 seconds.
	// Every co2_read takes 5 seconds, so we need to do 180 / 5 = 36 co2_reads.
	for (int i = 0; i < 36; i++)
	{
		uint16_t values[3];
		esp_err_t err = co2_read(address, values);
		if (err != ESP_OK)
		{
			ESP_LOGW("CO2", "CRC was incorrect or no sensor wat attached.");
			return ESP_ERR_INVALID_RESPONSE;
		}
	}

	// Generate command buffer:
	uint8_t x[2] = {SCD41_CO2_RECALIBRATION_VAL};
	uint8_t force_recalibrate_cmd[5] = {SCD41_CMD_FORCE_RECALIBRATION,
										SCD41_CO2_RECALIBRATION_VAL,
										scd41_crc8(x, 2)}; // Command buffer with generated CRC, write 0x019F to FORCE_RECALIBRATION address.
	// Write to I2C:
	twomes_i2c_write_port_1(address, force_recalibrate_cmd, sizeof force_recalibrate_cmd, I2C_SEND_STOP);

	vTaskDelay(500 / portTICK_PERIOD_MS); // Give SCD41 time for processing. 400ms according to the datasheet, + margin.
	// Read the result:
	uint8_t response_buffer[3];
	twomes_i2c_read_port_1(address, response_buffer, sizeof response_buffer);

	uint8_t crc = scd41_crc8(response_buffer, 2);
	if (crc != response_buffer[2])
		return ESP_ERR_INVALID_CRC;

	// Debug print:
	ESP_LOGD("Force recalibrate SCD41", "Received Response: %2X, %2X", response_buffer[0], response_buffer[1]);

	uint16_t response = ((int16_t)response_buffer[0] << 8) | response_buffer[1];
	if (response == 0xffff)
		return ESP_ERR_INVALID_RESPONSE;

	response -= 0x8000; // We need to substract 0x8000 according to the datasheet.

	*offset = (int16_t)(response);
	return ESP_OK;
}

esp_err_t co2_read(uint8_t address, uint16_t *buffer)
{
	// Send singleshot command:
	uint8_t singleshot_cmd[2] = {SCD41_CMD_SINGLESHOT};
	twomes_i2c_write_port_1(address, singleshot_cmd, sizeof singleshot_cmd, I2C_SEND_STOP);

	// wait
	ESP_LOGD("CO2", "wait %d ms", SCD41_SINGLE_SHOT_DELAY_MS);
	vTaskDelay(pdMS_TO_TICKS(SCD41_SINGLE_SHOT_DELAY_MS));

	// Read the measurement:
	uint8_t read_measurement_cmd[2] = {SCD41_CMD_READMEASURE};
	twomes_i2c_write_port_1(address, read_measurement_cmd, sizeof read_measurement_cmd, I2C_SEND_NO_STOP);
	vTaskDelay(SCD41_WAIT_MS / portTICK_PERIOD_MS);
	uint8_t read_buffer[9]; // Read 3 words (16 bits) and 3 CRCs (8 bits)
	twomes_i2c_read_port_1(address, read_buffer, sizeof read_buffer);

	// Perform CRC for each measurement:
	uint8_t crc1, crc2, crc3;
	crc1 = scd41_crc8(&read_buffer[0], 2);
	crc2 = scd41_crc8(&read_buffer[3], 2);
	crc3 = scd41_crc8(&read_buffer[6], 2);

	// If CRC matches, store value in buffer: TODO implement re-read when a crc fails
	if (crc1 == read_buffer[2])
	{
		buffer[0] = (read_buffer[0] << 8) | read_buffer[1]; // CO2
	}
	else
		return ESP_ERR_INVALID_CRC;

	if (crc2 == read_buffer[5])
	{
		buffer[1] = (read_buffer[3] << 8) | read_buffer[4]; // Temp
	}
	else
		return ESP_ERR_INVALID_CRC;

	if (crc3 == read_buffer[8])
	{
		buffer[2] = (read_buffer[6] << 8) | read_buffer[7]; // Humidity
	}
	else
		return ESP_ERR_INVALID_CRC;

	ESP_LOGD("CO2", "Measurement complete: CO2: 0x%02X%02X with CRC 0x%02X, T: 0x%02X%02X with CRC 0x%02X, RH 0x%02X%02X with CRC 0x%2X", read_buffer[0], read_buffer[1], read_buffer[2], read_buffer[3], read_buffer[4], read_buffer[5], read_buffer[6], read_buffer[7], read_buffer[8]);
	ESP_LOGD("CRC", "Calculated CRC1: 0x%02X, CRC2: 0x%02X, CRC3: 0x%02X", crc1, crc2, crc3);
	return ESP_OK;
}

float scd41_temp_raw_to_celsius(uint16_t raw)
{
	return ((float)-45 + (float)175 * (float)raw / 65536);
}
float scd41_rh_raw_to_fraction(uint16_t raw)
{
	return ((float)raw / 65536);
}
