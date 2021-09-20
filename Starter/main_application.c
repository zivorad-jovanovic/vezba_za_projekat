/* Standard includes. */
#include <stdio.h>
#include <conio.h>
#include <string.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"
#include "extint.h"

/* Hardware simulator utility functions */
#include "HW_access.h"

/* SERIAL SIMULATOR CHANNEL TO USE */
#define COM_CH0 (0)
#define COM_CH1 (1)

/* TASK PRIORITIES */
#define	led		( tskIDLE_PRIORITY + 5 )
#define	prijem1	( tskIDLE_PRIORITY + 6 )
#define	prijem0		( tskIDLE_PRIORITY + 4 )
#define	process		( tskIDLE_PRIORITY + 3 )
#define	send	( tskIDLE_PRIORITY + 2 )
#define	display	( tskIDLE_PRIORITY + 1 )



/* TASKS: FORWARD DECLARATIONS */
void led_bar_tsk(void* pvParameters);

void SerialReceive0_Task(void* pvParameters);
void SerialReceive1_Task(void* pvParameters);

void Processing_Task(void* pvParameters);
void Display_Task(void* pvParameters);
void led_bar_tsk(void* pvParameters);
void SerialSend_Task(void* pvParameters);

/* TRASNMISSION DATA - CONSTANT IN THIS APPLICATION */
static const char character[] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x06D, 0x7D, 0x07, 0x7F, 0x6F };

typedef struct AD    //napravi se struktura sa poljima cije vrednosti se menjaju(podaci sa senzora brzine i senzora prozora - TIJANA)
{
	uint8_t ADmin;
	uint8_t ADmax;
	float value;
	float current_value;
	float max_value;
	float min_value;
}AD_izlaz;

AD_izlaz struktura = { 5, 15, 5, 0, 5 };
uint8_t stanje_auta = 0;
int kanal0 = 0, kanal1 = 0;
/* RECEPTION DATA BUFFER */   //ne treba
#define R_BUF_SIZE (32)
uint8_t r_buffer[R_BUF_SIZE];
unsigned volatile r_point;
static int naredba = 0;
int MAX = 0;
int taster_display_max = 0, taster_display_min = 0;

/* 7-SEG NUMBER DATABASE - ALL HEX DIGITS */     //ne treba
static const char hexnum[] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07,
								0x7F, 0x6F, 0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71 };

/* GLOBAL OS-HANDLES */
//SemaphoreHandle_t LED_INT_BinarySemaphore;
SemaphoreHandle_t TXC_BinarySemaphore;    //semafori kojima se odredjeni task obavestava da je nesto poslato sa serijske ili primljeno sa serijske
SemaphoreHandle_t RXC_BinarySemaphore0;	  // // slanje i primanje karaktera putem serijske se takodje vrsi pomocu interapta
SemaphoreHandle_t RXC_BinarySemaphore1;
SemaphoreHandle_t AD_BinarySemaphore;
SemaphoreHandle_t ispis_BinarySemaphore;
SemaphoreHandle_t LED_INT_BinarySemaphore;
SemaphoreHandle_t Send_BinarySemaphore;
SemaphoreHandle_t Display_BinarySemaphore;

QueueHandle_t data_queue, dataAD_queue;                  // red kojim ce se slati podaci izmedju taskova
TimerHandle_t tajmer_displej;
/*
static uint32_t prvProcessTBEInterrupt(void)    //svaki put kad se nesto posalje sa serijske desi se interrupt i predaje se semafor
{
	BaseType_t xHigherPTW = pdFALSE;
	xSemaphoreGiveFromISR(TXC_BinarySemaphore, &xHigherPTW);     // kad se desi interrupt predaj semafor
	portYIELD_FROM_ISR(xHigherPTW);
}
*/
static uint32_t prvProcessRXCInterrupt(void)    //svaki put kad se nesto posalje sa serijske desi se interrupt i predaje se semafor
{
	BaseType_t xHigherPTW = pdFALSE;
	if (get_RXC_status(0) != 0) {// vraca nenultu vrednost ako je dosao interrupt sa kanala nula
		xSemaphoreGiveFromISR(RXC_BinarySemaphore0, &xHigherPTW);     // kad se desi interrupt predaj semafor
		kanal0 = 1;
		kanal1 = 0;
	}
	else if (get_RXC_status(1) != 0) {// vraca nenultu vrednost ako je dosao interrupt sa kanala jeddan
		xSemaphoreGiveFromISR(RXC_BinarySemaphore1, &xHigherPTW);
		kanal0 = 0;
		kanal1 = 1;
	}
	portYIELD_FROM_ISR(xHigherPTW);
}

static uint32_t OnLED_ChangeInterrupt(void) {    //u interaptu samo predamo semafor

	BaseType_t higher_priority_task_woken = pdFALSE;

	xSemaphoreGiveFromISR(LED_INT_BinarySemaphore, &higher_priority_task_woken); //ako se sa give probudio task viseg prioriteta onda odmah izvrsi scheduling!

	portYIELD_FROM_ISR(higher_priority_task_woken);
}

void TimerCallBack(void* pvParameters)//upitno dal je STATIC
{

	if (send_serial_character((uint8_t)COM_CH0, (uint8_t)'T') != 0) { //SLANJE TRIGGER SIGNALA
		printf("Greska_send \n");
	}

	static uint32_t brojac3 = 0;

	brojac3++;

	if (brojac3 == (uint32_t)20) {
		brojac3 = (uint32_t)0;
		if (xSemaphoreGive(Send_BinarySemaphore, 0) != pdTRUE) {
			printf("GRESKA");
		}
	}


	if (xSemaphoreGive(Display_BinarySemaphore, 0) != pdTRUE) {
		printf("DISPLAY GRESKA SEMAFOR\n");
	}

}


void Display_Task(void* pvParameters)   //prikaz vrednosti AD konvertora na displej
{

	uint8_t pom;
	uint8_t jedinice, desetice, stotine;
	static uint8_t i = 0, j = 0, k = 0, tmp_cifra, tmp_cifra1, z, l;
	static uint16_t tmp_broj = 0, tmp_broj1 = 0;
	static uint16_t tmp_broj2 = 0, tmp_cifra2 = 0;

	while (1)
	{

		if (xSemaphoreTake(Display_BinarySemaphore, portMAX_DELAY) != pdTRUE) {  //brojac  2-7  200ms*5=1s za osvjezavanje displeja
			printf("Greska take\n");
		}




		tmp_broj = (uint8_t)struktura.current_value; //ispisujemo trenutnu brzinu
		i = 0;
		if (tmp_broj < struktura.ADmin)
			tmp_broj = struktura.ADmin;
		printf("CURRENT VALUE %d\n", tmp_broj);
		for (z = 6; z <= 8; z++) {
			if (select_7seg_digit((uint8_t)z) != 0) {   //selektujemo od 4. poziciju
				printf("Greska_select \n");
			}
			if (set_7seg_digit(0x00) != 0) {      //upisemo od 123 3
				printf("Greska_set \n");
			}
		}
		if (tmp_broj == 0) {
			if (select_7seg_digit((uint8_t)8) != 0) {     //selektujemo od 8. cifre
				printf("Greska_select \n");
			}
			if (set_7seg_digit(hexnum[0]) != 0) {       //upisujemo od 456 6
				printf("Greska_set \n");
			}
		}
		else {
			while (tmp_broj != (uint8_t)0) {

				tmp_cifra = (uint8_t)tmp_broj % (uint8_t)10;

				if (select_7seg_digit((uint8_t)8 - i) != 0) {   //selektujemo od 4. poziciju
					printf("Greska_select \n");
				}
				if (set_7seg_digit(hexnum[tmp_cifra]) != 0) {      //upisemo od 123 3
					printf("Greska_set \n");
				}
				tmp_broj = tmp_broj / (uint8_t)10;  //dobili 12...i tako se nastavlja u krug while
				i++;
			}
		}

		if (taster_display_max == (uint8_t)1) {        //kada je pritisnut taster za displej na led baru

			taster_display_max = 0;
			tmp_broj1 = struktura.max_value;
			j = 0;
			if (tmp_broj1 == 0) {
				if (select_7seg_digit((uint8_t)5) != 0) {     //selektujemo od 8. cifre
					printf("Greska_select \n");
				}
				if (set_7seg_digit(hexnum[0]) != 0) {       //upisujemo od 456 6
					printf("Greska_set \n");
				}
			}
			else {
				while (tmp_broj1 != (uint8_t)0) {
					tmp_cifra1 = (uint8_t)tmp_broj1 % (uint8_t)10;
					if (select_7seg_digit((uint8_t)5 - j) != 0) {     //selektujemo od 8. cifre
						printf("Greska_select \n");
					}
					if (set_7seg_digit(hexnum[tmp_cifra1]) != 0) {       //upisujemo od 456 6
						printf("Greska_set \n");
					}
					tmp_broj1 = tmp_broj1 / (uint16_t)10;      //dobili 45...i tako se nastavlja u krug
					j++;

				}
			}
		}


		if (taster_display_min == (uint8_t)1) {        //kada je pritisnut taster za displej na led baru

			taster_display_min = 0;
			tmp_broj2 = struktura.min_value;
			for (l = 0; l <= 2; l++) {
				if (select_7seg_digit((uint8_t)l) != 0) {   //selektujemo od 4. poziciju
					printf("Greska_select \n");
				}
				if (set_7seg_digit(0x00) != 0) {      //upisemo od 123 3
					printf("Greska_set \n");
				}
			}
			k = 0;
			if (tmp_broj2 == 0) {
				if (select_7seg_digit((uint8_t)2) != 0) {     //selektujemo od 8. cifre
					printf("Greska_select \n");
				}
				if (set_7seg_digit(hexnum[0]) != 0) {       //upisujemo od 456 6
					printf("Greska_set \n");
				}
			}
			else {
				while (tmp_broj2 != (uint8_t)0) {
					tmp_cifra2 = (uint8_t)tmp_broj2 % (uint8_t)10;
					if (select_7seg_digit((uint8_t)2 - k) != 0) {     //selektujemo od 8. cifre
						printf("Greska_select \n");
					}
					if (set_7seg_digit(hexnum[tmp_cifra2]) != 0) {       //upisujemo od 456 6
						printf("Greska_set \n");
					}
					tmp_broj2 = tmp_broj2 / (uint16_t)10;      //dobili 45...i tako se nastavlja u krug
					k++;
				}
			}
		}



	}
}


void SerialSend_Task(void* pvParameters)
{

	static char vrednost[5];
	int broj;


	static uint16_t tmp_broj, tmp_broj_decimale;
	static uint8_t i = 0, k = 0, j = 0, cifra1, cifra2;
	static uint8_t tmp_cifra = 0;



	static char tmp_str[50], tmp_str1[10];
	char string_kontinualno[6], string_kontrolisano[6];
	string_kontinualno[0] = 'k';
	string_kontinualno[1] = 'o';
	string_kontinualno[2] = 'n';
	string_kontinualno[3] = 't';
	string_kontinualno[4] = 'i';
	string_kontinualno[5] = 32;


	string_kontrolisano[0] = 'k';
	string_kontrolisano[1] = 'o';
	string_kontrolisano[2] = 'n';
	string_kontrolisano[3] = 't';
	string_kontrolisano[4] = 'r';
	string_kontrolisano[5] = 32;
	while (1)
	{
		xSemaphoreTake(Send_BinarySemaphore, portMAX_DELAY);      //ovde se ceka na semafor
		//printf("posle semafora send\n");
		if (naredba == 0) {
			for (i = 0; i <= (uint8_t)5; i++) {
				{ //KONTINUALNO
					if (send_serial_character((uint8_t)COM_CH1, (uint8_t)string_kontinualno[i]) != 0) { //SLANJE PROZORA
						printf("Greska_send \n");
					}
					vTaskDelay(pdMS_TO_TICKS(100));

				}


			}

		}
		else if (naredba == 1) {
			for (i = 0; i <= (uint8_t)5; i++) {
				{ //KONTINUALNO
					if (send_serial_character((uint8_t)COM_CH1, (uint8_t)string_kontrolisano[i]) != 0) { //SLANJE PROZORA
						printf("Greska_send \n");
					}
					vTaskDelay(pdMS_TO_TICKS(100));

				}


			}

		}
		k = (uint8_t)0;

		tmp_broj = (uint16_t)struktura.value; //123  char
		tmp_broj_decimale = struktura.value * 100 - 100 * tmp_broj; //5.18 -> 518- 100 *5
		//printf("Value %d\n", tmp_broj);
		if (tmp_broj == 0) {
			if (send_serial_character((uint8_t)COM_CH1, 48) != 0) { //SLANJE PROZORA
				printf("Greska_send \n");
			}
			vTaskDelay(pdMS_TO_TICKS(100));



			if (send_serial_character((uint8_t)COM_CH1, (uint8_t)13) != 0) { //SLANJE PROZORA
				printf("Greska_send \n");
			}
		}
		else {
			while (tmp_broj != (uint16_t)0) {
				tmp_cifra = (uint8_t)tmp_broj % (uint8_t)10; //3, 2
				tmp_broj = tmp_broj / (uint16_t)10; //12
				tmp_str1[k] = tmp_cifra + (char)48; // 3 2 1  int
				k++;
			}
			j = 1;

			if (k != (uint8_t)0) {    //obrne ga kad ga salje
				while (k != (uint8_t)0) {

					if (send_serial_character((uint8_t)COM_CH1, (uint8_t)tmp_str1[k - j]) != 0) { //SLANJE PROZORA
						printf("Greska_send \n");
					}
					k--;

					vTaskDelay(pdMS_TO_TICKS(100));
				}


			}


			printf("tmp_broj_decimale %d \n", tmp_broj_decimale);
			if (send_serial_character((uint8_t)COM_CH1, (uint8_t)46) != 0) { //TACKA
				printf("Greska_send \n");
			}
			vTaskDelay(pdMS_TO_TICKS(100));

			cifra1 = tmp_broj_decimale / 10;  // 18 /10 = 1
			cifra2 = tmp_broj_decimale % 10;
			//printf("CIFRA1 %d, cifra2 %d \n", cifra1, cifra2);
			if (send_serial_character((uint8_t)COM_CH1, 48 + cifra1) != 0) { //SLANJE PROZORA
				printf("Greska_send \n");
			}
			vTaskDelay(pdMS_TO_TICKS(100));

			if (send_serial_character((uint8_t)COM_CH1, 48 + cifra2) != 0) { //SLANJE PROZORA
				printf("Greska_send \n");
			}
			vTaskDelay(pdMS_TO_TICKS(100));



			if (send_serial_character((uint8_t)COM_CH1, (uint8_t)13) != 0) { //SLANJE PROZORA
				printf("Greska_send \n");
			}
			i = 0;
		}
		//xSemaphoreTake(ispis_BinarySemaphore, portMAX_DELAY);

	}
}



static void SerialReceive0_Task()     // ovaj task samo primi karakter sa serijske a zatim ga preko reda prosledi ka tasku za obradu
{
	uint8_t cc;
	static char tmp_str[200], string_queue[200];
	static uint8_t k;
	static uint8_t z = 0;
	int len;
	for (;;)
	{
		//printf("task 2\n");
		//predavanje ce biti na svakih 200ms
		if (xSemaphoreTake(RXC_BinarySemaphore0, portMAX_DELAY) != pdTRUE) {
			printf("Greska take\n");
		}
		if (get_serial_character(COM_CH0, &cc) != 0) {
			printf("Greskaget1 \n");
		}
		//printf("karakter koji pristize %c\n", cc);
		if (cc != (uint8_t)43) { //0123+
			tmp_str[z] = (char)cc;
			z++;

		}
		else {
			tmp_str[z] = '\0';
			z = 0;
			//printf("String sa serijske, nulti %s, %c \n", tmp_str, tmp_str[0]);
			len = (uint8_t)strlen(tmp_str) % (uint8_t)12;
			for (k = 0; k < len; k++) {
				string_queue[k] = tmp_str[k];
				tmp_str[k] = "";
			}
			string_queue[len] = '\0';
			//printf("String **************** %s \n", string_queue);
			if (xQueueSend(data_queue, &string_queue, 0) != pdTRUE) {
				printf("Greskared, slanje\n");
			}
			//printf("Red za task 3 \n");
		}
	}

}

static void SerialReceive1_Task()     // ovaj task samo primi karakter sa serijske a zatim ga preko reda prosledi ka tasku za obradu
{
	uint8_t cc1 = 0;
	char tmp_str[100], string_queue[100];
	static uint8_t i = 0, tmp;

	while (1)
	{
		if (xSemaphoreTake(RXC_BinarySemaphore1, portMAX_DELAY) != pdTRUE) {
			printf("Greska em take1 \n");
		}
		//printf("Ako smo kliknuli na send text - kanal 1\n");
		if (get_serial_character(COM_CH1, &cc1) != 0) {
			printf("Greska_get\n");
		}
		printf("karakter koji pristize %c\n", cc1);
		if (cc1 != (uint8_t)43) {
			if (cc1 >= (uint8_t)65 && cc1 <= (uint8_t)90) { //velika slova prebacujemo u mala
				tmp = cc1 + (uint8_t)32;
				tmp_str[i] = (char)tmp;
				i++;
			}
			else {
				tmp_str[i] = (char)cc1;
				i++;
			}
		}
		else {
			tmp_str[i] = '\0';
			i = 0;
			printf("String sa serijske %s \n", tmp_str);
			strcpy(string_queue, tmp_str);
			if (xQueueSend(data_queue, &string_queue, 0) != pdTRUE) {
				printf("Greska_get\n");
			}
			printf("Red za task 3 \n");
		}

	}

}




void Processing_Task(void* pvParameters)   //obrada podataka
{

	char niz[7];
	uint8_t pom;
	static int vrednost = 0, brojac = 0;
	double suma_uV = 0;
	float suma1;

	while (1)
	{

		//prvo primate red
		xQueueReceive(data_queue, &niz, portMAX_DELAY);     // ovde se prima podatak iz serial receive taska


															//ovo je string
		//kONTINU
		//KONTROL
		//ADMIN15
		//ADMAX26
		//broj od cetiri cifre
		//printf("NAREDBA %d\n", naredba);
		float pomocna_suma = 0;
		if (niz[0] == 'a' && niz[1] == 'd' && niz[2] == 'm' && niz[3] == 'i' && niz[4] == 'n')
		{
			struktura.ADmin = (niz[5] - 48) * 10 + (niz[6] - 48);
			printf("ADmin = %d\n", struktura.ADmin);     // u polju strukture ADmin se sada nalazi data vrednost
		}

		else if (niz[0] == 'a' && niz[1] == 'd' && niz[2] == 'm' && niz[3] == 'a' && niz[4] == 'x')
		{
			struktura.ADmax = (niz[5] - 48) * 10 + (niz[6] - 48);

			printf("ADmax = %d\n", struktura.ADmax);    // u polju strukture ADmax se sada nalazi data vrednost
		}

		else if (niz[0] == 'k' && niz[1] == 'o' && niz[2] == 'n' && niz[3] == 't' && niz[4] == 'i' && niz[5] == 'n' && niz[6] == 'u')
		{

			printf("Poslali ste kontinualno\n");    // u polju strukture ADmax se sada nalazi data vrednost
			//xQueueSemaphoreTake(LED_INT_BinarySemaphore, portMAX_DELAY);
			if (stanje_auta == 1) {
				//punjenje naponsko
				set_LED_BAR(1, 0x07);

			}
			naredba = 0;
		}

		else if (niz[0] == 'k' && niz[1] == 'o' && niz[2] == 'n' && niz[3] == 't' && niz[4] == 'r' && niz[5] == 'o' && niz[6] == 'l')
		{
			printf("Poslali ste kontrolisano\n");     // u polju strukture ADmax se sada nalazi data vrednost
			naredba = 1;


		}
		else if (niz[0] == 48 || niz[0] == 49) {



			int brr = 0, cifra, suma = 0;
			while (brr < 4) {
				if (niz[brr] >= 48 && niz[brr] <= 57) {
					cifra = niz[brr] - 48;
					//	printf("CIFRA %d\n", cifra);
					suma = suma * 10 + cifra;
					suma1 = (struktura.ADmax - struktura.ADmin) * suma;
				}
				brr++;
			}

			suma_uV = suma1 / (float)1023 + (float)struktura.ADmin;
			if (suma_uV < struktura.ADmin)
				suma_uV = struktura.ADmin;
			struktura.current_value = suma_uV;

			if (struktura.current_value > struktura.max_value) {
				struktura.max_value = struktura.current_value;
			}
			if (struktura.current_value < struktura.min_value) {
				struktura.min_value = struktura.current_value;
			}
			if (naredba == 1) {

				printf("Vrednost: %lf\n", suma_uV);
				//suma_uV = (struktura.ADmax - struktura.ADmin) * suma / 1023 + struktura.ADmin;
				//printf("Vrednost: %.2f\n", suma_uV);
				if (suma_uV < 12.5) {
					//pali strujno
					if (stanje_auta == 1)
						set_LED_BAR(1, 0X0B);
					else
						set_LED_BAR(1, 0X0A);
				}
				else if (suma_uV > 13.5 && suma_uV < 14) {
					if (stanje_auta == 1)
						set_LED_BAR(1, 0X07);
					else
						set_LED_BAR(1, 0X06);
				}
				else if (suma_uV >= 14) {
					if (stanje_auta == 1)
						set_LED_BAR(1, 0X01);
					else
						set_LED_BAR(1, 0X00);
				}
			}
			if (brojac < 20) {
				vrednost = vrednost + suma;
				brojac++;
			}
			else {
				pomocna_suma = vrednost / 20;
				struktura.value = (struktura.ADmax - struktura.ADmin) * pomocna_suma / 1023 + struktura.ADmin;
				brojac = 0;
				vrednost = 0;
				printf("Usrednjena vrednsot: %.2f\n", struktura.value);

			}
			//struktura.value = (struktura.ADmax - struktura.ADmin) * suma / 1023 + struktura.ADmin;
			//printf("Obicna vrednsot: %.2f\n", struktura.value);


		}
		else if (niz[0] == 'L') {
			printf("LEDOVKE\n");

			stanje_auta = (uint8_t)niz[1] - (uint8_t)48;  //nezavisno od toga da li rezim 0 ili 1
			set_LED_BAR(1, stanje_auta);

			taster_display_min = (uint8_t)niz[2] - (uint8_t)48;
			taster_display_max = (uint8_t)niz[3] - (uint8_t)48;




		}


		//struktura.value = obrada.value;
		//xQueueSend(dataAD_queue, &obrada, 0);   //saljemo podatak o trenutnoj vrednosti akumulatora
	}

}


void led_bar_tsk(void* pvParameters) {

	uint8_t led_tmp, tmp_cifra, led_tmp1, i;

	static char tmp_string[20];

	for (;;)
	{
		xSemaphoreTake(LED_INT_BinarySemaphore, portMAX_DELAY);   // ovde task ceka da se desi prekid 
		   //pomocna promenjiva u koju se smesta stanje dioda sa cetvrtog stubca
		if (get_LED_BAR((uint8_t)0, &led_tmp) != 0) { // 0000, 0001, 0010, 0011, .....,1111
			printf("Greska_get\n");
		}

		//	get_LED_BAR(0, &pom);
			//set_LED_BAR(1, pom);


		printf("LED_TMP %d \n", led_tmp);
		led_tmp1 = led_tmp; //1101
		tmp_string[0] = 'L';
		for (i = 1; i <= 3; i++) {
			tmp_cifra = led_tmp1 % 2;
			led_tmp1 = led_tmp1 / 2;
			tmp_string[i] = tmp_cifra + 48;
		}
		tmp_string[4] = '\0';
		printf("STRING LEDOVKE >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> %s \n", tmp_string);
		if (xQueueSend(data_queue, &tmp_string, 0) != pdTRUE) {
			printf("Greska_get\n");
		}
		/*	if (pom == 1)
				stanje_auta = 1;//automobil je  ukljucen
			else
				stanje_auta = 0;*/

	}

}


void main_demo(void)
{
	// Init peripherals
	int i;
	init_LED_comm();
	init_7seg_comm();
	init_serial_uplink(COM_CH0); // inicijalizacija serijske TX na kanalu 0
	init_serial_downlink(COM_CH0);// inicijalizacija serijske RX na kanalu 0

	init_serial_uplink(COM_CH1); // inicijalizacija serijske TX na kanalu 1
	init_serial_downlink(COM_CH1);// inicijalizacija serijske RX na kanalu 1


								 /* LED BAR INTERRUPT HANDLER */
	vPortSetInterruptHandler(portINTERRUPT_SRL_OIC, OnLED_ChangeInterrupt);  //omogucava inerapt ii definise fju za obradu prekida 

	/* SERIAL TRANSMISSION INTERRUPT HANDLER */
	//vPortSetInterruptHandler(portINTERRUPT_SRL_TBE, prvProcessTBEInterrupt);       //preko interrupta se salje sa serijske veze kao prijemnom tasku

	/* SERIAL RECEPTION INTERRUPT HANDLER */
	vPortSetInterruptHandler(portINTERRUPT_SRL_RXC, prvProcessRXCInterrupt);


	// Semaphores
	RXC_BinarySemaphore0 = xSemaphoreCreateBinary();
	RXC_BinarySemaphore1 = xSemaphoreCreateBinary();
	TXC_BinarySemaphore = xSemaphoreCreateBinary();
	AD_BinarySemaphore = xSemaphoreCreateBinary();
	ispis_BinarySemaphore = xSemaphoreCreateBinary();
	LED_INT_BinarySemaphore = xSemaphoreCreateBinary();
	Display_BinarySemaphore = xSemaphoreCreateBinary();
	Send_BinarySemaphore = xSemaphoreCreateBinary();

	// Queues
	data_queue = xQueueCreate(1, 7 * sizeof(char));
	dataAD_queue = xQueueCreate(2, sizeof(AD_izlaz));

	// Timers

	BaseType_t status;
	tajmer_displej = xTimerCreate(     //pomocu tajmera cemo definisati na koliko se poziva fja tajmera(svakih 200ms -> Tijana)
		"timer",
		pdMS_TO_TICKS(100),
		pdTRUE,
		NULL,
		TimerCallBack
	);


	//tasks

	xTaskCreate(SerialReceive0_Task, "receive_task", configMINIMAL_STACK_SIZE, NULL, prijem0, NULL);  //task koji prima podatke sa serijske
	xTaskCreate(SerialReceive1_Task, "receive_task", configMINIMAL_STACK_SIZE, NULL, prijem1, NULL);  //task koji prima podatke sa serijske
	xTaskCreate(SerialSend_Task, "send_task", configMINIMAL_STACK_SIZE, NULL, send, NULL);  //task koji SALJE podatke na serijsku svakih 1s
	xTaskCreate(Processing_Task, "processing_task", configMINIMAL_STACK_SIZE, NULL, process, NULL);  //task za obradu podataka
	xTaskCreate(led_bar_tsk, "led_task", configMINIMAL_STACK_SIZE, NULL, led, NULL);  //task za obradu podataka
	xTaskCreate(Display_Task, "display", configMINIMAL_STACK_SIZE, NULL, (UBaseType_t)display, NULL);

	/* SERIAL TRANSMITTER TASK */
	//xTaskCreate(SerialSend_Task, "STx", configMINIMAL_STACK_SIZE, NULL, TASK_SERIAL_SEND_PRI, NULL);
	//r_point = 0;

	xTimerStart(tajmer_displej, 0);
	struktura.min_value = struktura.ADmax;
	for (i = 0; i <= 8; i++) {
		if (select_7seg_digit((uint8_t)i) != 0) {     //selektujemo od 8. cifre
			printf("Greska_select \n");
		}
		if (set_7seg_digit(0x00) != 0) {       //upisujemo od 456 6
			printf("Greska_set \n");
		}
	}
	vTaskStartScheduler();
	while (1);
}
