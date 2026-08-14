import type { Messages } from '../types'

// Serbian (Latin script / latinica). Typed as `Messages`, so a key missing from here — or a key
// here that no longer exists in `en.ts` — fails `vue-tsc` rather than rendering blank.
//
// IEC 61850 vocabulary (GOOSE, MMS, RCB, LN, SCL, ICD/CID, CIDR, host, port) is protocol
// vocabulary and is deliberately left untranslated, matching how substation engineers here
// actually write it.
export const sr: Messages = {
  app: {
    name: 'Station Signal',
  },

  nav: {
    scan: 'Skeniranje',
    devices: 'Uređaji',
    reports: 'Izveštaji',
    settings: 'Podešavanja',
  },

  theme: {
    switchToLight: 'Prebaci na svetlu temu',
    switchToDark: 'Prebaci na tamnu temu',
  },

  language: {
    switchTo: 'Promeni jezik na {language}',
  },

  common: {
    connect: 'Poveži',
    cancel: 'Otkaži',
    stop: 'Zaustavi',
    view: 'Prikaži',
    clear: 'Očisti',
    remove: 'Ukloni',
    apply: 'Primeni',
    retryNow: 'Pokušaj odmah',
    checkNow: 'Proveri odmah',
    loading: 'Učitavanje…',
    none: '(nema)',
    empty: '—',
    unexpectedError: 'Neočekivana greška',
    showAdvanced: 'Prikaži napredne opcije',
    hideAdvanced: 'Sakrij napredne opcije',
    advanced: 'Napredno',
  },

  fields: {
    host: 'Host',
    mmsPort: 'MMS port',
    interface: 'Interfejs',
    iedName: 'Naziv IED-a',
    optional: 'opciono',
    interfacePlaceholder: 'eth0',
    hostPlaceholder: '10.250.99.14',
    interfaceRequired: 'Interfejs je obavezan.',
    hostRequired: 'Host je obavezan.',
    portRange: 'Port mora biti ceo broj između 1 i 65535.',
  },

  shortcuts: {
    label: 'Prečice',
    click: 'klik',
    connectDefault: 'preskoči izbor, poveži se na Control + Other',
    connectAll: 'preskoči izbor, poveži se na sve kategorije',
    connectTooltip: 'Shift + klik: poveži se na Control + Other · Ctrl + klik: poveži se na sve kategorije',
  },

  scan: {
    title: 'Skeniranje mreže',
    subtitle:
      'Pretražite jedan ili više mrežnih interfejsa u potrazi za IED uređajima koji govore MMS. Više skeniranja može raditi istovremeno.',
    startScan: 'Pokreni skeniranje',
    noScans: 'Nema pokrenutih skeniranja.',
    scanNumber: 'Skeniranje #{id}',
    portSummary: '· port {port}',
    status: {
      starting: 'Pokretanje skeniranja…',
      active: 'Skeniranje je aktivno — osluškuju se pronađeni hostovi.',
      stopping: 'Zaustavljanje skeniranja…',
      retryingIn: 'Novi pokušaj za {seconds}s…',
      interrupted: 'Veza je prekinuta — neki rezultati možda nedostaju.',
    },
    results: {
      discovered: 'Pronađen',
      empty: 'Nijedan host još nije pronađen.',
    },
  },

  devices: {
    title: 'Praćeni uređaji',
    subtitle: 'Uređaji sa kojih se trenutno izveštava, ažurirano uživo.',
    status: 'Status',
    lastMessage: 'Poslednja poruka',
    emptyBefore: 'Nema praćenih uređaja. Povežite se na neki iznad ili pokrenite jedan sa stranice',
    emptyLink: 'Skeniranje mreže',
    emptyAfter: '.',
    phase: {
      connecting: 'Povezivanje…',
      connected: 'Povezan',
      interrupted: 'Prekinuto',
      interruptedRetrying: 'Prekinuto — ponovni pokušaj…',
      stopping: 'Zaustavljanje…',
      error: 'Greška',
    },
  },

  reports: {
    title: 'Izveštaji',
    subtitle: 'GOOSE/MMS izveštaji uživo za svaki uređaj koji se trenutno prati.',
    emptyBefore: 'Nema praćenih uređaja. Povežite se na neki sa stranice',
    emptyDevicesLink: 'Uređaji',
    emptyMiddle: 'ili pokrenite jedan sa stranice',
    emptyScanLink: 'Skeniranje mreže',
    emptyAfter: '.',
    stopReporting: 'Zaustavi izveštavanje',
    interfaceAndPhase: 'Interfejs {interfaceId} — {phase}',
    table: {
      time: 'Vreme',
      type: 'Tip',
      reference: 'Referenca',
      value: 'Vrednost',
      quality: 'Kvalitet',
      empty: 'Nijedan izveštaj još nije primljen.',
      emptyForFilter: 'Nijedan izveštaj ne odgovara izabranim kategorijama.',
      buffered: '{rcb} (baferovano)',
    },
    capability: {
      noMms: 'Ovaj uređaj nema MMS report control blokove u svom SCL-u — ovde će biti prikazan samo GOOSE.',
      noGoose: 'Ovaj uređaj nema GOOSE control blokove u svom SCL-u — ovde će biti prikazani samo MMS izveštaji.',
    },
    filter: {
      label: 'Kategorije',
      all: 'Sve',
      hint: 'Filtrira ono što se prikazuje u ovom pregledaču. Ne menja na šta je uređaj pretplaćen.',
    },
    shared: {
      banner:
        'Ovaj uređaj je već povezala druga sesija, pretplaćena na {categories}. Vi gledate taj deljeni tok — kategorije koje ste izabrali nisu primenjene. Koristite filter ispod da suzite prikaz.',
      allCategories: 'sve kategorije',
    },
  },

  connectPrompt: {
    passwordLabel: '{host} zahteva lozinku',
    passwordRequired: 'Lozinka je obavezna.',
    failed: 'Povezivanje sa uređajem nije uspelo.',
  },

  categoryModal: {
    title: 'Na šta želite da se povežete?',
    subtitle: 'Izaberite kategorije logičkih čvorova (LN) na koje se pretplaćujete na ovom uređaju.',
    all: 'Poveži se na sve kategorije',
    selectAtLeastOne: 'Izaberite bar jednu kategoriju ili odaberite „Poveži se na sve kategorije”.',
    control: 'Upravljanje',
    controlHint: 'Prekidači, rastavljači, nadzorno upravljanje',
    measurement: 'Merenje',
    measurementHint: 'Analogne i merne vrednosti',
    protection: 'Zaštita',
    protectionHint: 'Funkcije zaštitnih releja',
    other: 'Ostalo',
    otherHint: 'Sistemski logički čvorovi i sve nekategorisano',
  },

  structureFile: {
    label: 'Datoteka strukture (SCL/ICD/CID)',
    uploading: 'Otpremanje…',
    dropHint: 'Prevucite datoteku strukture ovde ili kliknite da izaberete',
    extensions: '.icd, .cid, .scd, .xml',
    uploadFailed: 'Otpremanje datoteke strukture nije uspelo.',
    iedNameRequired: 'Naziv IED-a je obavezan kada je zadata putanja datoteke strukture.',
  },

  settings: {
    title: 'Podešavanja',
    subtitle:
      'Promenite mrežnu adresu ovog uređaja. Ovo je podešavanje celog uređaja, nije vezano za vašu sesiju u pregledaču — primenjuje se isto za svakog tehničara koji ga koristi.',
    current: {
      heading: 'Trenutna mrežna konfiguracija',
      interface: 'Interfejs',
      address: 'Adresa',
      gateway: 'Mrežni prolaz',
    },
    form: {
      heading: 'Promena statičke IP adrese',
      addressLabel: 'Nova adresa',
      addressPlaceholder: '172.16.0.50',
      addressHint: 'Prefiks je opcion — ako ga izostavite, podrazumeva se {prefix}.',
      addressPreview: 'Biće primenjeno kao {cidr}',
      addressInvalid: 'Unesite ispravnu IPv4 adresu, npr. 172.16.0.50 ili 172.16.0.50/24.',
      gatewayLabel: 'Mrežni prolaz',
      gatewayPlaceholder: '192.168.1.1',
      gatewayInvalid: 'Unesite ispravnu IPv4 adresu, npr. 192.168.1.1.',
      gatewayAdvanced: 'Mrežni prolaz',
      prefillCurrent: 'Popuni trenutnim',
      provisionalNote:
        'Primena je privremena: ako ova stranica ne uspe da potvrdi da je uređaj dostupan na novoj adresi u zadatom roku, uređaj se sam vraća na prethodnu konfiguraciju — pogledajte napomenu o oporavku ispod ako to ikada treba uraditi ručno.',
    },
    phase: {
      applying: 'Primena nove mrežne konfiguracije…',
      waitingBefore: 'Primenjeno — čeka se odgovor uređaja na adresi',
      waitingOr: '(ili',
      waitingAfter: ').',
      checkingIn: 'Ponovna provera za {seconds}s…',
      autoRevertNote:
        'Ako uređaj ne potvrdi dostupnost na vreme, sam će se vratiti na prethodnu adresu — nije potrebna nikakva radnja.',
      confirming: 'Potvrđivanje…',
      confirmedBefore: 'Potvrđeno — preusmeravanje na',
      reverting: 'Poništavanje promene na čekanju i vraćanje prethodne adrese…',
      reverted:
        'Uređaj nije potvrdio dostupnost na vreme. Trebalo bi da se sam vratio na prethodnu konfiguraciju — osvežite ovu stranicu da proverite. Ako i dalje ne odgovara, upotrebite „Poništi promenu na čekanju” ispod ili fiksnu adresu za oporavak.',
      alreadyPending:
        'Ovaj uređaj još drži otvorenu raniju promenu mreže, pa nova ne može biti primenjena. Ako to nije promena koju čekate, poništite je ispod.',
    },
    attempts: {
      heading: 'Pokušaji povezivanja',
      ok: 'odgovorio je i dozvolio ovoj stranici da pročita odgovor',
      httpError: 'odgovorio je, ali sa greškom — proksi radi, a API iza njega možda ne',
      blockedByCors: 'odgovorio je, ali je odbio ovoj stranici dozvolu da pročita odgovor (CORS)',
      unreachable: 'niko nije odgovorio na toj adresi',
      timeout: 'prihvatio je vezu ali nije odgovorio na vreme — pokušava se ponovo',
    },
    pending: {
      heading: 'Promena mreže je još na čekanju',
      withAddressBefore: 'Ovaj uređaj drži otvorenu adresu',
      withAddressAfter:
        'i čeka potvrdu. Dok se ne potvrdi ili poništi, nijedna nova adresa ne može biti primenjena.',
      withoutAddress:
        'Ovaj uređaj drži otvorenu raniju promenu. Dok se ne poništi, nijedna nova adresa ne može biti primenjena.',
      clear: 'Poništi promenu na čekanju',
    },
    recovery: {
      heading: 'Ako uređaj postane nedostupan',
      bodyBefore: 'Svaki uređaj trajno nosi fiksnu adresu za oporavak na',
      bodyAfter:
        'nezavisnu od adrese podešene iznad i nikada je ova stranica ne menja. Povežite laptop direktno na uređaj (ili preko sviča na kome nema ničega drugog), ručno podesite svoj mrežni adapter na statičku IP adresu u istom opsegu — npr. {example}, mrežni prolaz nije potreban — pa otvorite tu adresu.',
    },
    advanced: {
      heading: 'Napredno',
      clearLogsDescription:
        'Prazni log fajlove na ovom ure\u0111aju. Koristite neposredno pre testiranja, kako bi logovi koje posle preuzmete sadr\u017eali samo tu sesiju.',
      clearLogs: 'Obri\u0161i log fajlove',
      clearing: 'Brisanje\u2026',
      cleared: 'Obrisano {count} log fajl(ova), oslobo\u0111eno {size}.',
      clearedWithSkipped: 'Obrisano {count} log fajl(ova), oslobo\u0111eno {size}. {skipped} nije moglo biti obrisano.',
      clearedNothing: 'Nema log fajlova za brisanje.',
      clearFailed: 'Neuspe\u0161no brisanje log fajlova',
      logDir: 'Direktorijum sa logovima',
    },
  },

  errors: {
    connectionRejected: 'Veza je odbijena — možda je potrebna lozinka',
  },
}
