
#include <xc.h>
#include <string.h>
#include <stdio.h>
#include "gpio.h"
#include "lcd.h"
#include "usb/usb.h"

//__delay-hez
#include <libpic30.h>

//KVANTalas eteke
#define KVANT 20

void cvref_init(){
    // feszültségreferencia konfigurálása
    CVRCONbits.CVREN = 1; // feszültségreferencia engedélyezése
    CVRCONbits.CVROE = 1; // feszültségreferencia kimenet engedélyezése
    CVRCONbits.CVRR = 0; // tartomány: 0..Vdd    
    CVRCONbits.CVRSS = 0; // Vdd és GND közötti értékek
    CVRCONbits.CVR = 0b1000; // fokozat: 8 -> 1/2 Vdd (1.65V)
}

// ADC konfigurálás manuális mintavétel, automatikus konverzió
void adc_init_auto_conversion(){
    // analóg lábak kiválasztása
    AD1PCFGL=0xfffd; // AN1 láb analóg funkciójának kiválasztása
    
    // üzemmód
    AD1CON1bits.ASAM = 0; // manuális mintavételezés
    AD1CON1bits.SSRC = 0b111; // !!! automatikus konverzió indítás
    
    // mérési tartomány
    AD1CON2bits.VCFG = 0b000; // ref+=Vdd, ref-=Vss
    
    // időzítés
    AD1CON3bits.ADRC = 0; // órajel forrás: CPU (Tcy)
    AD1CON3bits.ADCS = 1; // konverzió időegység: Tad = 2*Tcy = 125ns > 75ns
    AD1CON3bits.SAMC = 31; // 31 Tad
    
    // bekapcsolás
    AD1CON1bits.ADON = 1; // ADC bekapcsolása
    
    // csatorna kiválasztás
    AD1CHSbits.CH0SA = 1; // AN1 kiválasztása
}

// tömbök USB kommunikációhoz
char readBuffer[64]; // fogadott adatok
char writeBuffer[64]; // küldendő adatok

// USB kommunikáció kezelése
void USB_comm_task() {
    // ha nincs konfigurálva az USB, akkor nincs további teendő
    if( USBGetDeviceState() < CONFIGURED_STATE )
        return;

    // ha fel van függesztve az USB kommunikáció, akkor nincs további teendő
    if(USBIsDeviceSuspended())
        return;

    // csak akkor kezeljük az USB eszközt, ha minden küldeni kívánt adat kiment
    if(USBUSARTIsTxTrfReady()){
        // bejövő adat olvasása (n=fogadott byteok száma)
        int n = getsUSBUSART((uint8_t*)readBuffer, sizeof(readBuffer));

        // van fogadott adat?
        if(n>0){
            // válasz küldése
            //sprintf(writeBuffer, "Kaptam %d byte adatot\n", n);  //KOMMENT
            putUSBUSART((uint8_t*)writeBuffer,strlen(writeBuffer));
        }
    }

    // adatküldés service hívása
    CDCTxService();
}

// sor beolvasása
int line_len=0;
char line[100];

void process_line(){
    putUSBUSART((uint8_t*)writeBuffer,strlen(writeBuffer));
}

enum STATES{
    State_init,
    State_rpm,
    State_volt
};

int main(){
    // rendszer órajel postscaler inicializálása és watchdog timer letiltása
    CLKDIVbits.CPDIV = 0; 
    while(!OSCCONbits.LOCK) Nop();
    RCONbits.SWDTEN = 0;
    
    // GPIO (gombok, LED-ek) inicializálása
    gpio_init();
    
    //meres init
    cvref_init();
    adc_init_auto_conversion();
    
    // LCD inicializálása
    lcdInit();
    
    // USB inicializálása és várakozás sikeres csatlakozásra
    USBDeviceInit(); 
    USBDeviceAttach();
    
    // üdvözlő üzenet
    lcdPutStr("Szögsebesség\nmérés");
    __delay_ms(2000);
    
    lcdClear();
    sprintf(LCD, "SW1:Szögsebesség\nSW2:Feszültség");
    lcdPutStr(LCD);

    //kezdeti állapot
    enum STATES State = State_init;
    
    double OFFSET_FESZ = 1.65;
    int szamlalo = 0;
    int tacho_tomb[KVANT];
    for (int i=0; i<KVANT; i++){
                tacho_tomb[i] = 512;
            }
    long long int tacho_fesz_szum = 0;
    
    // főciklus
    while(1){
        __delay_ms(10);
        USB_comm_task();
        
        process_line();
        
        
        AD1CON1bits.SAMP = 1; // mintavételezés indítás
        while(!AD1CON1bits.DONE); // várakozás konverzióra
        int tacho = ADC1BUF0; // mérési eredmény
        
        //mozgó átlag
        if (szamlalo < KVANT) tacho_tomb[szamlalo] = tacho;
        else {
            for (int i=0; i<KVANT-1; i++){
                tacho_tomb[i] = tacho_tomb[i+1];
            }
            tacho_tomb[KVANT-1] = tacho;
            
        }
        
        tacho_fesz_szum = 0;
        for (int i=0; i<KVANT; i++){
            tacho_fesz_szum += tacho_tomb[i];
        }
        
        double tacho_fesz_szum_db = 1.0*tacho_fesz_szum;
        double tacho_fesz_atlag_dc = tacho_fesz_szum_db/KVANT;
        double tacho_fesz_atlag = (tacho_fesz_atlag_dc/1023.0) * 3.3 - OFFSET_FESZ;
        
        double fordulatszam_atlag = (tacho_fesz_atlag / 0.52) * 1000;

        double szogsebesseg = (fordulatszam_atlag * 2 * 3.1415) / 60;
        
        sprintf(writeBuffer, "\n%.5f", szogsebesseg);
        
        if(USBUSARTIsTxTrfReady()){
            putUSBUSART((uint8_t*)writeBuffer,strlen(writeBuffer));
            printf(writeBuffer);
        }
        
        switch (State){
            case State_init:
                if (SW1) {
                    __delay_ms(5);
                    State = State_rpm;
                }
                
                if (SW2) {
                    __delay_ms(5);
                    State = State_volt;
                }
                
                break;
                
            
            case State_rpm:
                if (szamlalo % 25 == 0) {
                    lcdClear();
                    sprintf(LCD, "Szögsebesség:\n%5.0f [rad/s]", szogsebesseg);
                    lcdPutStr(LCD);
                }
                break;
                
            case State_volt:
                if (szamlalo % 25 == 0) {
                    lcdClear();
                    sprintf(LCD, "Feszültség:\n%6.3f [V]", tacho_fesz_atlag);
                    lcdPutStr(LCD);   
                }
                break;
            
        }
        szamlalo++;
    }
    
    return 0;
}