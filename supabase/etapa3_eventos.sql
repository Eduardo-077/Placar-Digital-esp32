-- ETAPA 3 - Eventos da partida e telemetria (Fase 4 do roteiro_ferramentas.txt)
-- Colar inteiro no SQL Editor do Supabase e clicar em Run.
-- Pode rodar várias vezes sem erro.
--
-- O ESP grava aqui cada gol/ponto/falta (tabela eventos) e, a cada 60 s,
-- a "saúde" do placar (tabela telemetria). O Node-RED lê as duas e
-- copia para o InfluxDB.

-- 1) Token do placar: igual ao PIN, mas é o ESP que usa (vai no segredos.h)
alter table public.segredos add column if not exists token text unique;

update public.segredos
set token = replace(gen_random_uuid()::text, '-', '')
where token is null;

-- 2) Tabela dos eventos do jogo
create table if not exists public.eventos (
  id           bigint generated always as identity primary key,
  placar_id    text not null,
  partida_id   bigint references public.partidas (id) on delete set null,
  modo         text not null
               check (modo in ('futebol', 'basquete', 'volei', 'tenis', 'jiujitsu', 'padel')),
  tipo         text not null
               check (tipo in ('inicio', 'ponto', 'correcao', 'falta', 'vantagem', 'punicao',
                               'play', 'pausa', 'fim_periodo', 'fim')),
  time         text check (time in ('A', 'B')),       -- null = não é de um time só
  placar_a     int not null,
  placar_b     int not null,
  sets_a       int not null default 0,              -- vôlei, tênis e padel
  sets_b       int not null default 0,
  periodo      int not null default 1,              -- tempo, quarto ou set
  tempo_jogo_s int not null default 0,              -- relógio do período, em segundos
  created_at   timestamptz not null default now()
);

alter table public.eventos enable row level security;

drop policy if exists "leitura publica" on public.eventos;
create policy "leitura publica" on public.eventos
  for select to anon using (true);

revoke all on public.eventos from anon, authenticated;
grant select on public.eventos to anon;

-- 3) Tabela da telemetria (a cada 60 s)
create table if not exists public.telemetria (
  id         bigint generated always as identity primary key,
  placar_id  text not null,
  rssi       int not null,                          -- sinal do Wi-Fi em dBm (-30 ótimo, -90 péssimo)
  heap_livre int not null,                          -- memória livre em bytes
  uptime_s   bigint not null,                       -- segundos desde que ligou
  luz        int not null,                          -- sensor de luz (IO34): 0 a 4095
  created_at timestamptz not null default now()
);

alter table public.telemetria enable row level security;

drop policy if exists "leitura publica" on public.telemetria;
create policy "leitura publica" on public.telemetria
  for select to anon using (true);

revoke all on public.telemetria from anon, authenticated;
grant select on public.telemetria to anon;

-- 4) Funções que conferem o token e gravam. São a única forma de inserir.
--    O placar_id vem do token, o ESP não precisa mandar.
drop function if exists public.registrar_evento(text, bigint, text, text, text, int, int, int, int, int, int);

create function public.registrar_evento(
  p_token text, p_partida_id bigint, p_modo text, p_tipo text, p_time text,
  p_placar_a int, p_placar_b int, p_sets_a int, p_sets_b int,
  p_periodo int, p_tempo_jogo_s int
)
returns bigint
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_placar text;
  novo_id  bigint;
begin
  select placar_id into v_placar from public.segredos where token = p_token;
  if v_placar is null then
    raise exception 'Token incorreto';
  end if;

  -- partida apagada / id que não existe: grava sem ligar a uma partida
  if not exists (select 1 from public.partidas where id = p_partida_id) then
    p_partida_id := null;
  end if;

  insert into public.eventos (placar_id, partida_id, modo, tipo, time,
                              placar_a, placar_b, sets_a, sets_b, periodo, tempo_jogo_s)
  values (v_placar, p_partida_id, p_modo, p_tipo, p_time,
          p_placar_a, p_placar_b, p_sets_a, p_sets_b, p_periodo, p_tempo_jogo_s)
  returning id into novo_id;

  return novo_id;
end;
$$;

revoke execute on function public.registrar_evento(text, bigint, text, text, text, int, int, int, int, int, int) from public, authenticated;
grant execute on function public.registrar_evento(text, bigint, text, text, text, int, int, int, int, int, int) to anon;

drop function if exists public.registrar_telemetria(text, int, int, bigint, int);

create function public.registrar_telemetria(
  p_token text, p_rssi int, p_heap_livre int, p_uptime_s bigint, p_luz int
)
returns bigint
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_placar text;
  novo_id  bigint;
begin
  select placar_id into v_placar from public.segredos where token = p_token;
  if v_placar is null then
    raise exception 'Token incorreto';
  end if;

  insert into public.telemetria (placar_id, rssi, heap_livre, uptime_s, luz)
  values (v_placar, p_rssi, p_heap_livre, p_uptime_s, p_luz)
  returning id into novo_id;

  return novo_id;
end;
$$;

revoke execute on function public.registrar_telemetria(text, int, int, bigint, int) from public, authenticated;
grant execute on function public.registrar_telemetria(text, int, int, bigint, int) to anon;

-- 5) Avisa a API do Supabase que as tabelas e funções mudaram
notify pgrst, 'reload schema';

-- Depois de rodar: copiar o token para o PLACAR_TOKEN do segredos.h
--   select placar_id, token from public.segredos;

-- Testes (rodar um bloco por vez; troque TOKEN pelo valor de cima):
-- Token errado -> deve dar "Token incorreto"
--   set role anon;
--   select public.registrar_evento('errado', null, 'futebol', 'ponto', 'A', 1, 0, 0, 0, 1, 120);
--   reset role;
-- Token certo -> deve retornar o id do evento
--   set role anon;
--   select public.registrar_evento('TOKEN', null, 'futebol', 'ponto', 'A', 1, 0, 0, 0, 1, 120);
--   select public.registrar_telemetria('TOKEN', -60, 150000, 3600, 2000);
--   reset role;
-- Gravar direto como anon -> deve dar "permission denied"
--   set role anon;
--   insert into public.eventos (placar_id, modo, tipo, placar_a, placar_b) values ('x', 'futebol', 'ponto', 9, 9);
--   reset role;
-- Conferir e limpar os testes:
--   select * from public.eventos order by id desc limit 5;
--   delete from public.eventos;  delete from public.telemetria;
