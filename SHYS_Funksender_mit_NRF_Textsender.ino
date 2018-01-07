/* 
 homecontrol incl. Intertechno BT-Switch Support - v1.2
 
 Arduino Sketch fÃ¼r homecontrol 
 Copyright (c) 2016 Daniel Scheidler All right reserved.
 
 homecontrol ist Freie Software: Sie kÃ¶nnen es unter den Bedingungen
 der GNU General Public License, wie von der Free Software Foundation,
 Version 3 der Lizenz oder (nach Ihrer Option) jeder spÃ¤teren
 verÃ¶ffentlichten Version, weiterverbreiten und/oder modifizieren.
 
 homecontrol wird in der Hoffnung, dass es nÃƒÂ¼tzlich sein wird, aber
 OHNE JEDE GEWÃ„HRLEISTUNG, bereitgestellt; sogar ohne die implizite
 GewÃƒÂ¤hrleistung der MARKTFÃ„HIGKEIT oder EIGNUNG FÃœR EINEN BESTIMMTEN ZWECK.
 Siehe die GNU General Public License fÃ¼r weitere Details.
 
 Sie sollten eine Kopie der GNU General Public License zusammen mit diesem
 Programm erhalten haben. Wenn nicht, siehe <http://www.gnu.org/licenses/>.
 */
#include <SPI.h>
#include <Ethernet.h>
#include <RCSwitch.h>
#include <SoftwareSerial.h>
#include <RF24.h> 
#include <DigitalIO.h> // https://github.com/greiman/DigitalIO   

// Patch file RF24_config.h in RF24 Library to enable softspi to use NRF with Ethernetshield
// - uncomment (remove //) #define SOFTSPI
// - change pin numbers as below:
//      const uint8_t SOFT_SPI_MISO_PIN = 15;
//      const uint8_t SOFT_SPI_MOSI_PIN = 14;
//      const uint8_t SOFT_SPI_SCK_PIN = 16;
// - Use Pins A0-A2 for Mosi(A0), Miso(A1) and SCK(A2) instead of 11-13


/*
 * NRF PINS:
 * ------------
 * A0   MOSI
 * A1   MISO
 * A2   SCK
 * 9    CE
 * 10   CSN
 * 
 * 433MHz TX Pin
 * ---------------
 * 7    DATA 
 * 
 * 433MHz RX Pin
 * ---------------
 * 2    DATA 
 */


// ---------------------------------------------------------------
// --                      START CONFIG                         --
// ---------------------------------------------------------------
#define TRANSMITTER_PIN 7      // Transmitter is connected to Arduino Pin #7  
#define RECEIVER_PIN 0         //Receiver is on Interrupt 0 - Arduino Pin #2 

// RF24 
// ClientNummer 2 (0xF0F0F0F0D2LL) wird von der Uhr als anzuzeigende Nachricht ausgewertet
byte ClientNummer = 2; // Mögliche Werte: 1-6
static const uint64_t pipes[6] = {0xF0F0F0F0E1LL, 0xF0F0F0F0D2LL, 0xF0F0F0F0C3LL, 0xF0F0F0F0B4LL, 0xF0F0F0F0A5LL, 0xF0F0F0F096LL};

// Anzahl wie oft das Signal des NRF gesendet werden soll (um Verlust des Signals auszuschließen)
int RF_SEND_REPEAT = 3;
// ---------------------------------------------------------------
// --                       END CONFIG                          --
// ---------------------------------------------------------------
bool switched_on = false;
unsigned long tprev = 0;


// Netzwerkdienste
EthernetServer HttpServer(80); 
EthernetClient interfaceClient;

// Webseiten/Parameter
char*      rawCmdAnschluss          = (char*)malloc(sizeof(char)*10);
char*      rawCmdDimmLevel          = (char*)malloc(sizeof(char)*10);
char       rawCmdText[50]           = "";
const int  MAX_BUFFER_LEN           = 80; // max characters in page name/parameter 
char       buffer[MAX_BUFFER_LEN+1]; // additional character for terminating null


// Instance of RCSwitch
RCSwitch mySwitch = RCSwitch();

#if defined(__SAM3X8E__)
    #undef __FlashStringHelper::F(string_literal)
    #define F(string_literal) string_literal
#endif

const __FlashStringHelper * htmlHeader;
const __FlashStringHelper * htmlHead;
const __FlashStringHelper * htmlFooter;

// ------------------ Reset stuff --------------------------
void(* resetFunc) (void) = 0;
unsigned long resetMillis;
boolean resetSytem = false;
// --------------- END - Reset stuff -----------------------


RF24 radio(9,10);


char* on = "ON";
char* off= "OFF";


/**
 * SETUP
 *
 * Grundeinrichtung der HomeControl
 * - Serielle Ausgabe aktivieren
 * - TFT initialisieren
 * - Netzwerk initialisieren
 * - Webserver starten
 * - IN-/OUT- Pins definieren
 * - Waage initialisieren (Tara)
 */
void setup() {
  unsigned char mac[]  = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED  };
  unsigned char ip[]   = { 192, 168, 1, 13 };
  unsigned char dns[]  = { 192, 168, 1, 1  };
  unsigned char gate[] = { 192, 168, 1, 1  };
  unsigned char mask[] = { 255, 255, 255, 0  };
  
  // Serial initialisieren
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for Leonardo only
  }
  Serial.println(F("HomeControl v1"));
  Serial.println();

  mySwitch.enableTransmit(TRANSMITTER_PIN);
  mySwitch.enableReceive(RECEIVER_PIN);
 
  // Optional set pulse length.
  //mySwitch.setPulseLength(350);  

  // Netzwerk initialisieren
  Ethernet.begin(mac, ip, dns, gate, mask);
  HttpServer.begin();

  Serial.print( F("IP: ") );
  Serial.println(Ethernet.localIP());

  initStrings();
  
  radio.begin();
  delay(20);

  radio.setChannel(1);                // Funkkanal - Mögliche Werte: 0 - 127
  radio.setAutoAck(0);
  radio.setRetries(15,15);    
  radio.setPALevel(RF24_PA_HIGH);     // Sendestärke darf die gesetzlichen Vorgaben des jeweiligen Landes nicht überschreiten! 
                                      // RF24_PA_MIN=-18dBm, RF24_PA_LOW=-12dBm, RF24_PA_MED=-6dBM, and RF24_PA_HIGH=0dBm
  radio.setDataRate(RF24_1MBPS);                                  

  radio.openWritingPipe(pipes[ClientNummer-1]);
  radio.openReadingPipe(1, pipes[0]); 
 
  radio.startListening();
}

/**
 * LOOP
 * 
 * Standard-Loop-Methode des Arduino Sketch
 *
 * - Webserver: 
 *    * auf Client warten
 *      * Falls Client verbunden ist entsprechende Webseite ausgeben
 *        und Verbindung beenden.
 */
void loop() {
  EthernetClient client = HttpServer.available();
  if (client) {
    while (client.connected()) {
      if(client.available()){        

        Serial.println(F("Website anzeigen"));

        showWebsite(client);
        
        delay(100);
        client.stop();
      }
    }
  }
  delay(100);
  // Gecachte URL-Parameter leeren
  memset(rawCmdAnschluss,0, sizeof(rawCmdAnschluss)); 
  memset(rawCmdDimmLevel,0, sizeof(rawCmdDimmLevel)); 
  strcpy(rawCmdText,"");
}



void sendMessage(){
  radio.stopListening(); 
  delay(100);
  for(int i=0;i<RF_SEND_REPEAT;i++){
    radio.write(&rawCmdText, sizeof(rawCmdText));
    delay(100);     
  }
  Serial.print("Output: ");
  Serial.println(rawCmdText);
  
  radio.txStandBy();          // Need to drop out of TX mode every 4ms if sending a steady stream of multicast data
  delayMicroseconds(130);     // This gives the PLL time to sync back up   

  radio.startListening();
}
 




// ---------------------------------------
//     RcSwitch Hilfsmethoden
// ---------------------------------------
/**
 * Schaltet die GerÃƒÂ¤te per Funk / BT 
 * 
 * 1-50    = 433-MHz Funkstandard (Funksteckdosen mit Dip-Schalter)
 * 51-306  = Intertechno 433MHz Drehrad-Kodierung von A-01 bis P-16
 * 307-386 = Intertechno BT-Switch 
 */
void switchWirelessOutlet(int number){
   switchWirelessOutlet(number, 0);
}

void switchOnOff( char switchCode, int switchId, boolean switchOn, int numberStk){
  if(switchOn){
    mySwitch.switchOn(switchCode, switchId, numberStk);  
  } else {
    mySwitch.switchOff(switchCode,switchId, numberStk);  
  }
}

void switchWirelessOutlet(int number, int dimm){
  mySwitch.disableReceive();


  Serial.print(F("Schalte: "));
  Serial.println(number);

  delay(10);

  char dimmLevel[5];
  sprintf(dimmLevel,"%02i",dimm);

  int numberStk = number % 5;  
  if (numberStk == 0) numberStk = 5;
  if (numberStk < 0) numberStk = numberStk*(-1);
  
  int numberStkIT = (number-50) % 4;
  if (numberStkIT == 0) numberStkIT = 4;
  if (numberStkIT < 0) numberStkIT = numberStkIT*(-1);
  
  int switchNr = number>=0?number:number * (-1);
  
  int switchId = 0;
  boolean switchOn = true;
  
  if (switchNr > 290){
    switchOnOff('p',((((switchNr-290)-1)/4)+1), number>=0, numberStkIT);  
  } else if (switchNr > 274){
    switchOnOff('o',((((switchNr-274)-1)/4)+1), number>=0, numberStkIT);   
  } else if (switchNr > 258){
    switchOnOff('n',((((switchNr-258)-1)/4)+1), number>=0, numberStkIT);  
  } else if (switchNr > 242){
    switchOnOff('m',((((switchNr-242)-1)/4)+1), number>=0, numberStkIT);   
  } else if (switchNr > 226){
    switchOnOff('l',((((switchNr-226)-1)/4)+1), number>=0, numberStkIT);    
  } else if (switchNr > 210){
    switchOnOff('k',((((switchNr-210)-1)/4)+1), number>=0, numberStkIT);    
  } else if (switchNr > 194){
    switchOnOff('j',((((switchNr-194)-1)/4)+1), number>=0, numberStkIT);   
  } else if (switchNr > 178){
    switchOnOff('i',((((switchNr-178)-1)/4)+1), number>=0, numberStkIT);    
  } else if (switchNr > 162){
    switchOnOff('h',((((switchNr-162)-1)/4)+1), number>=0, numberStkIT);    
  } else if (switchNr > 146){
    switchOnOff('g',((((switchNr-146)-1)/4)+1), number>=0, numberStkIT);    
  } else if (switchNr > 130){
    switchOnOff('f',((((switchNr-130)-1)/4)+1), number>=0, numberStkIT);    
  } else if (switchNr > 114){
    switchOnOff('e',((((switchNr-114)-1)/4)+1), number>=0, numberStkIT);   
  } else if (switchNr > 98){
    switchOnOff('d',((((switchNr-98)-1)/4)+1), number>=0, numberStkIT);   
  } else if (switchNr > 82){
    switchOnOff('c',((((switchNr-82)-1)/4)+1), number>=0, numberStkIT);    
  } else if (switchNr > 66){
    switchOnOff('b',((((switchNr-66)-1)/4)+1), number>=0, numberStkIT);   
  } else if (switchNr > 50){
    switchOnOff('a',((((switchNr-50)-1)/4)+1), number>=0, numberStkIT);    
  } else {
    //Standard Logik
  if(number>=0){
      mySwitch.switchOff(int2bin(((switchNr-1)/5)+1), numberStk);
   } else {
      mySwitch.switchOn(int2bin(((switchNr-1)/5)+1), numberStk);
   }
  } 

  delay(10);
  mySwitch.enableReceive(0);
}


// ---------------------------------------
//     Webserver Hilfsmethoden
// ---------------------------------------
/**
 *  URL auswerten und entsprechende Seite aufrufen
 */
void showWebsite(EthernetClient client){
  char * HttpFrame =  readFromClient(client);
  
 // delay(200);
  boolean pageFound = false;
  
  char *ptr = strstr(HttpFrame, "favicon.ico");
  if(ptr){
    pageFound = true;
  }
  ptr = strstr(HttpFrame, "index.html");
  if (!pageFound && ptr) {
    runIndexWebpage(client);
    pageFound = true;
  } 
  ptr = strstr(HttpFrame, "rawCmd");
  if(!pageFound && ptr){
    runRawCmdWebpage(client, HttpFrame);
    pageFound = true;
  } 

  delay(200);

  ptr=NULL;
  HttpFrame=NULL;

 if(!pageFound){
    runIndexWebpage(client);
  }
  delay(20);
}




// ---------------------------------------
//     Webseiten
// ---------------------------------------

/**
 * Startseite anzeigen
 */
void  runIndexWebpage(EthernetClient client){
  showHead(client);

  client.print(F("<h4>Navigation</h4><br/>"
    "<a href='/rawCmd'>Manuelle Schaltung</a><br>"));

  showFooter(client);
}


/**
 * rawCmd anzeigen
 */
void  runRawCmdWebpage(EthernetClient client, char* HttpFrame){
  if (atoi(rawCmdAnschluss)!=0 || strcmp(rawCmdText, "")!=0 ) {
    postRawCmd(client, rawCmdAnschluss);
    return;
  }

  showHead(client);
  
  client.println(F(  "<h4>Manuelle Schaltung</h4><br/>"
                     "<form action='/rawCmd'>"));

  client.println(F( "<b>Anschluss: </b>" 
                    "<select name=\"schalte\" size=\"1\" > "));
  
  for(int i=0; i<=386;i++){
    client.print(F( "  <option value=\""));
    client.print(i); 
    client.print(F( "\">Anschluss "));
    client.print(i);
    client.println(F( "</option>"));
  }

  client.println(F( "</select>" ));
  client.println(F( "<input type='submit' value='Abschicken'/>"
                    "</form>"));

  showFooter(client);
}


void postRawCmd(EthernetClient client, char* anschluss){
  showHead(client);
    
  client.println(F( "<h4> Funk schalten</h4><br/>" ));
  if(atoi(rawCmdDimmLevel)>0 && atoi(rawCmdDimmLevel)<17){
    switchWirelessOutlet(atoi(anschluss), atoi(rawCmdDimmLevel));
  } else if(atoi(anschluss)>0){
    switchWirelessOutlet(atoi(anschluss));
  }

  if( strcmp(rawCmdText, "")!=0){
    sendMessage();
  }
  showFooter(client);
}





// ---------------------------------------
//     HTML-Hilfsmethoden
// ---------------------------------------

void showHead(EthernetClient client){
  client.println(htmlHeader);
  client.print("IP: ");
  client.println(Ethernet.localIP());
  client.println(htmlHead);
}


void showFooter(EthernetClient client){
  client.println(F("<div  style=\"position: absolute;left: 30px; bottom: 40px; text-align:left;horizontal-align:left;\" width=200>"));
  client.print(F("<b>Freier Speicher:</b> "));                              
  client.println(F("</div>"));
  client.print(htmlFooter);
}


void initStrings(){
  htmlHeader = F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n");
  
  htmlHead = F("<html><head>"
    "<title>HomeControl</title>"
    "<style type=\"text/css\">"
    "body{font-family:sans-serif}"
    "*{font-size:14pt}"
    "a{color:#abfb9c;}"
    "</style>"
    "</head><body text=\"white\" bgcolor=\"#494949\">"
    "<center>"
    "<hr><h2>HomeControl</h2><hr>") ;
    
    htmlFooter = F( "</center>"
    "<a  style=\"position: absolute;left: 30px; bottom: 20px; \"  href=\"/\">Zurueck zum Hauptmenue;</a>"
    "</body></html>");
    
}


// ---------------------------------------
//     Ethernet - Hilfsmethoden
// ---------------------------------------
/**
 * Zum auswerten der URL des ÃƒÂ¼bergebenen Clients
 * (implementiert um angeforderte URL am lokalen Webserver zu parsen)
 */
char* readFromClient(EthernetClient client){
  char paramName[20];
  char paramValue[20];
  char pageName[20];
  
  if (client) {
  
    while (client.connected()) {
  
      if (client.available()) {
        memset(buffer,0, sizeof(buffer)); // clear the buffer

        client.find("/");
        
        if(byte bytesReceived = client.readBytesUntil(' ', buffer, sizeof(buffer))){ 
          buffer[bytesReceived] = '\0';

          Serial.print(F("URL: "));
          Serial.println(buffer);
          
          if(strcmp(buffer, "favicon.ico\0")){
            char* paramsTmp = strtok(buffer, " ?=&/\r\n");
            int cnt = 0;
            
            while (paramsTmp) {
            
              switch (cnt) {
                case 0:
                  strcpy(pageName, paramsTmp);

                  Serial.print(F("Domain: "));
                  Serial.println(buffer);

                  break;
                case 1:
                  strcpy(paramName, paramsTmp);
                

                  Serial.print(F("Parameter: "));
                  Serial.print(paramName);

                  break;
                case 2:
                  strcpy(paramValue, paramsTmp);

                  Serial.print(F(" = "));
                  Serial.println(paramValue);

                  pruefeURLParameter(paramName, paramValue);
                  break;
              }
              
              paramsTmp = strtok(NULL, " ?=&/\r\n");
              cnt=cnt==0?1:cnt==1?2:1;
            }
          }
        }
      }// end if Client available
      break;
    }// end while Client connected
  } 

  return buffer;
}


void pruefeURLParameter(char* tmpName, char* value){
  if(strcmp(tmpName, "schalte")==0 && strcmp(value, "")!=0){
    strcpy(rawCmdAnschluss, value);
    
    Serial.print(F("Anschluss: "));
    Serial.println(rawCmdAnschluss);    
  }  
  if(strcmp(tmpName, "dimm")==0 && strcmp(value, "")!=0){
    strcpy(rawCmdDimmLevel, value);
    
    Serial.print(F("Dimm-Level: "));
    Serial.println(rawCmdAnschluss);    
  }  
  if(strcmp(tmpName, "text")==0 && strcmp(value, "")!=0){
    strcpy(rawCmdText, value);
    
    Serial.print(F("Text: "));
    Serial.println(rawCmdText);    
  }  
}

// ---------------------------------------
//     Allgemeine Hilfsmethoden
// ---------------------------------------
char* int2bin(unsigned int x){
  static char buffer[6];
  for (int i=0; i<5; i++) buffer[4-i] = '0' + ((x & (1 << i)) > 0);
  buffer[68] ='\0';
  return buffer;
}
