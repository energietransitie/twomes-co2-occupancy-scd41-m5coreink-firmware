#ifndef __SCD41_H__
#define __SCD41_H__

void scd41_init(void);
void scd41_disable_asc(void);
void scd41_measure_co2_temp_rht(void);
void scd41_reset_buffers(void);
void scd41_print_serial_number(void);

#endif // __SCD41_H__
