/* places.h - labels for the world map: continents and oceans (level 1), big countries (2), medium countries (3), cities (4).
 * lon, lat, level, English, Polish, short code (countries; used when the full name does not fit). Coordinates are rough centroids. */
#ifndef LT_PLACES_H
#define LT_PLACES_H
typedef struct { float lon, lat; int level; const char *en, *pl, *sh; } place;
static const place PLACES[] = {
	/* continents and oceans */
	{-100, 50, 1, "NORTH AMERICA", "AMERYKA PÓŁNOCNA", "N.AM."}, {-58, -21, 1, "SOUTH AMERICA", "AMERYKA POŁUDNIOWA", "S.AM."}, {0, 48, 1, "EUROPE", "EUROPA", "EUR"},
	{20, 6, 1, "AFRICA", "AFRYKA", "AFR"}, {95, 58, 1, "ASIA", "AZJA", "ASIA"}, {134, -26, 1, "AUSTRALIA", "AUSTRALIA", "AUS"}, {20, -80, 1, "ANTARCTICA", "ANTARKTYDA", "ANT"},
	{-145, 12, 1, "PACIFIC OCEAN", "OCEAN SPOKOJNY", "PAC"}, {-35, 22, 1, "ATLANTIC OCEAN", "OCEAN ATLANTYCKI", "ATL"}, {80, -22, 1, "INDIAN OCEAN", "OCEAN INDYJSKI", "IND"},
	{-10, 84, 1, "ARCTIC OCEAN", "OCEAN ARKTYCZNY", "ARC"}, {60, -62, 1, "SOUTHERN OCEAN", "OCEAN POŁUDNIOWY", "S.O."},
	/* big countries */
	{-105, 60, 2, "Canada", "Kanada", "CAN"}, {-100, 39, 2, "USA", "USA", "USA"}, {-102, 24, 2, "Mexico", "Meksyk", "MEX"}, {-53, -10, 2, "Brazil", "Brazylia", "BRA"},
	{-65, -35, 2, "Argentina", "Argentyna", "ARG"}, {-75, -9, 2, "Peru", "Peru", "PER"}, {-73, 4, 2, "Colombia", "Kolumbia", "COL"}, {-71, -30, 2, "Chile", "Chile", "CHL"},
	{95, 62, 2, "Russia", "Rosja", "RUS"}, {103, 35, 2, "China", "Chiny", "CHN"}, {79, 22, 2, "India", "Indie", "IND"}, {67, 48, 2, "Kazakhstan", "Kazachstan", "KAZ"},
	{3, 28, 2, "Algeria", "Algieria", "DZA"}, {17, 27, 2, "Libya", "Libia", "LBY"}, {30, 26, 2, "Egypt", "Egipt", "EGY"}, {30, 15, 2, "Sudan", "Sudan", "SDN"},
	{40, 8, 2, "Ethiopia", "Etiopia", "ETH"}, {24, -3, 2, "DR Congo", "Kongo", "COD"}, {17, -12, 2, "Angola", "Angola", "AGO"}, {25, -29, 2, "South Africa", "RPA", "ZAF"},
	{8, 9.5, 2, "Nigeria", "Nigeria", "NGA"}, {45, 24, 2, "Saudi Arabia", "Arabia Saudyjska", "SAU"}, {54, 32, 2, "Iran", "Iran", "IRN"}, {115, -2, 2, "Indonesia", "Indonezja", "IDN"},
	{-40, 73, 2, "Greenland", "Grenlandia", "GRL"},
	/* medium countries */
	{2, 46.5, 3, "France", "Francja", "FRA"}, {10.5, 51, 3, "Germany", "Niemcy", "DEU"}, {19.5, 52, 3, "Poland", "Polska", "POL"}, {-2, 54, 3, "United Kingdom", "Wielka Brytania", "GBR"},
	{-3.5, 40, 3, "Spain", "Hiszpania", "ESP"}, {12.5, 43, 3, "Italy", "Włochy", "ITA"}, {31, 49, 3, "Ukraine", "Ukraina", "UKR"}, {35, 39, 3, "Turkey", "Turcja", "TUR"},
	{16, 63, 3, "Sweden", "Szwecja", "SWE"}, {9, 62, 3, "Norway", "Norwegia", "NOR"}, {26, 64, 3, "Finland", "Finlandia", "FIN"}, {43, 33, 3, "Iraq", "Irak", "IRQ"},
	{69, 30, 3, "Pakistan", "Pakistan", "PAK"}, {66, 34, 3, "Afghanistan", "Afganistan", "AFG"}, {103, 47, 3, "Mongolia", "Mongolia", "MNG"}, {138, 37, 3, "Japan", "Japonia", "JPN"},
	{101, 15, 3, "Thailand", "Tajlandia", "THA"}, {106, 16, 3, "Vietnam", "Wietnam", "VNM"}, {96, 21, 3, "Myanmar", "Birma", "MMR"}, {122, 13, 3, "Philippines", "Filipiny", "PHL"},
	{-6, 32, 3, "Morocco", "Maroko", "MAR"}, {-4, 17, 3, "Mali", "Mali", "MLI"}, {9, 17, 3, "Niger", "Niger", "NER"}, {19, 15, 3, "Chad", "Czad", "TCD"},
	{35, -6, 3, "Tanzania", "Tanzania", "TZA"}, {38, 0.5, 3, "Kenya", "Kenia", "KEN"}, {35, -18, 3, "Mozambique", "Mozambik", "MOZ"}, {47, -19, 3, "Madagascar", "Madagaskar", "MDG"},
	{27, -14, 3, "Zambia", "Zambia", "ZMB"}, {17, -22, 3, "Namibia", "Namibia", "NAM"}, {24, -22, 3, "Botswana", "Botswana", "BWA"}, {-64, -17, 3, "Bolivia", "Boliwia", "BOL"},
	{-58, -23, 3, "Paraguay", "Paragwaj", "PRY"}, {-56, -33, 3, "Uruguay", "Urugwaj", "URY"}, {-78, -1.5, 3, "Ecuador", "Ekwador", "ECU"}, {-66, 7, 3, "Venezuela", "Wenezuela", "VEN"},
	{-79, 21.7, 3, "Cuba", "Kuba", "CUB"}, {172, -42, 3, "New Zealand", "Nowa Zelandia", "NZL"}, {145, -6, 3, "Papua New Guinea", "Papua-Nowa Gwinea", "PNG"}, {-19, 65, 3, "Iceland", "Islandia", "ISL"},
	{-8, 53, 3, "Ireland", "Irlandia", "IRL"}, {63, 41, 3, "Uzbekistan", "Uzbekistan", "UZB"}, {59, 39, 3, "Turkmenistan", "Turkmenistan", "TKM"}, {57, 21, 3, "Oman", "Oman", "OMN"},
	{47, 15.5, 3, "Yemen", "Jemen", "YEM"}, {105, 12.5, 3, "Cambodia", "Kambodża", "KHM"}, {90, 24, 3, "Bangladesh", "Bangladesz", "BGD"}, {84, 28, 3, "Nepal", "Nepal", "NPL"},
	{81, 7.5, 3, "Sri Lanka", "Sri Lanka", "LKA"}, {128, 36.5, 3, "South Korea", "Korea Płd.", "KOR"}, {127, 40, 3, "North Korea", "Korea Płn.", "PRK"}, {-84, 14, 3, "Honduras", "Honduras", "HND"},
	{28, 46, 3, "Romania", "Rumunia", "ROU"}, {22, 39, 3, "Greece", "Grecja", "GRC"}, {25, 53.5, 3, "Belarus", "Białoruś", "BLR"}, {14.5, 47.5, 3, "Austria", "Austria", "AUT"},
	{-8, 39.5, 3, "Portugal", "Portugalia", "PRT"}, {19, 47, 3, "Hungary", "Węgry", "HUN"}, {15.5, 49.8, 3, "Czechia", "Czechy", "CZE"}, {-2, 8, 3, "Ghana", "Ghana", "GHA"},
	{12, 4, 3, "Cameroon", "Kamerun", "CMR"}, {30, 1, 3, "Uganda", "Uganda", "UGA"}, {29, -19, 3, "Zimbabwe", "Zimbabwe", "ZWE"}, {102, 4, 3, "Malaysia", "Malezja", "MYS"},
	{35, 31, 3, "Israel", "Izrael", "ISR"}, {10, 34, 3, "Tunisia", "Tunezja", "TUN"}, {45, 42, 3, "Georgia", "Gruzja", "GEO"}, {66, 41.5, 3, "Kyrgyzstan", "Kirgistan", "KGZ"},
	/* cities */
	{21, 52.2, 2, "Warsaw", "Warszawa", ""}, {-0.1, 51.5, 4, "London", "Londyn", ""}, {2.3, 48.9, 4, "Paris", "Paryż", ""}, {13.4, 52.5, 4, "Berlin", "Berlin", ""},
	{-3.7, 40.4, 4, "Madrid", "Madryt", ""}, {12.5, 41.9, 4, "Rome", "Rzym", ""}, {37.6, 55.8, 4, "Moscow", "Moskwa", ""}, {30.5, 50.5, 4, "Kyiv", "Kijów", ""},
	{29, 41, 4, "Istanbul", "Stambuł", ""}, {31.2, 30, 4, "Cairo", "Kair", ""}, {36.8, -1.3, 4, "Nairobi", "Nairobi", ""}, {3.4, 6.5, 4, "Lagos", "Lagos", ""},
	{28, -26, 4, "Johannesburg", "Johannesburg", ""}, {55.3, 25.2, 4, "Dubai", "Dubaj", ""}, {77.2, 28.6, 4, "Delhi", "Delhi", ""}, {72.9, 19, 4, "Mumbai", "Mumbaj", ""},
	{116.4, 39.9, 4, "Beijing", "Pekin", ""}, {121.5, 31.2, 4, "Shanghai", "Szanghaj", ""}, {139.7, 35.7, 4, "Tokyo", "Tokio", ""}, {127, 37.6, 4, "Seoul", "Seul", ""},
	{103.8, 1.35, 4, "Singapore", "Singapur", ""}, {151.2, -33.9, 4, "Sydney", "Sydney", ""}, {106.8, -6.2, 4, "Jakarta", "Dżakarta", ""}, {-74, 40.7, 4, "New York", "Nowy Jork", ""},
	{-118.2, 34, 4, "Los Angeles", "Los Angeles", ""}, {-87.6, 41.9, 4, "Chicago", "Chicago", ""}, {-99.1, 19.4, 4, "Mexico City", "Miasto Meksyk", ""}, {-46.6, -23.5, 4, "Sao Paulo", "São Paulo", ""},
	{-58.4, -34.6, 4, "Buenos Aires", "Buenos Aires", ""}, {-77, -12, 4, "Lima", "Lima", ""}, {-74, 4.7, 4, "Bogota", "Bogota", ""}, {-79.4, 43.7, 4, "Toronto", "Toronto", ""},
	{-123, 49.3, 4, "Vancouver", "Vancouver", ""}, {-6.3, 53.3, 4, "Dublin", "Dublin", ""}, {18, 59.3, 4, "Stockholm", "Sztokholm", ""}, {10.7, 59.9, 4, "Oslo", "Oslo", ""},
	{19, 47.5, 4, "Budapest", "Budapeszt", ""}, {23.7, 38, 4, "Athens", "Ateny", ""}, {44.4, 33.3, 4, "Baghdad", "Bagdad", ""}, {51.4, 35.7, 4, "Tehran", "Teheran", ""},
	{-43, -22.9, 4, "Rio de Janeiro", "Rio de Janeiro", ""}, {18.4, -33.9, 4, "Cape Town", "Kapsztad", ""}, {-157.8, 21.3, 4, "Honolulu", "Honolulu", ""},
};
#endif
