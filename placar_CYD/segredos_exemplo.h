// Modelo do segredos.h - este arquivo vai para o GitHub, com valores falsos.
// Para usar: copie para "segredos.h" (mesma pasta) e preencha com os dados reais.
// O segredos.h verdadeiro está no .gitignore e nunca deve ser enviado.

#pragma once

// Wi-Fi (rede 2.4 GHz - o ESP32 não conecta em 5 GHz)
#define WIFI_NOME   "nome_da_rede"
#define WIFI_SENHA  "senha_da_rede"

// Supabase (Project Settings -> API Keys)
// Use SÓ a chave anon/publishable, NUNCA a service_role/secret.
#define SUPABASE_URL  "https://SEU_PROJETO.supabase.co"
#define SUPABASE_KEY  "sua_chave_anon_aqui"

// Identificador deste placar na tabela partidas
#define PLACAR_ID  "placar01"
