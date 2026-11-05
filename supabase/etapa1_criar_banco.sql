-- ETAPA 1 - Criação do banco do Placar Digital
-- Colar inteiro no SQL Editor do Supabase e clicar em Run.

-- 1) Tabela das partidas (o ESP lê daqui)
create table public.partidas (
  id         bigint generated always as identity primary key,
  placar_id  text not null default 'placar01',
  modo       text not null
             check (modo in ('futebol', 'basquete', 'volei', 'tenis', 'jiujitsu', 'padel')),
  time_a     text not null check (char_length(time_a) between 1 and 12),
  time_b     text not null check (char_length(time_b) between 1 and 12),
  created_at timestamptz not null default now()
);

alter table public.partidas enable row level security;

-- Qualquer um pode ler (nomes de times não são sensíveis)
create policy "leitura publica" on public.partidas
  for select to anon using (true);

revoke all on public.partidas from anon, authenticated;
grant select on public.partidas to anon;

-- 2) Tabela do PIN: RLS ligado, sem política e sem grant -> ninguém de fora lê
create table public.segredos (
  placar_id text primary key,
  pin       text not null
);

alter table public.segredos enable row level security;
revoke all on public.segredos from anon, authenticated;

-- TROQUE '1234' pelo PIN que você quiser
insert into public.segredos (placar_id, pin) values ('placar01', '1234');

-- 3) Função que confere o PIN e grava. É a única forma de inserir partidas.
create or replace function public.enviar_partida(
  p_pin text, p_modo text, p_time_a text, p_time_b text
)
returns bigint
language plpgsql
security definer
set search_path = ''
as $$
declare
  novo_id bigint;
begin
  if not exists (
    select 1 from public.segredos
    where placar_id = 'placar01' and pin = p_pin
  ) then
    raise exception 'PIN incorreto';
  end if;

  insert into public.partidas (placar_id, modo, time_a, time_b)
  values ('placar01', p_modo, trim(p_time_a), trim(p_time_b))
  returning id into novo_id;

  return novo_id;
end;
$$;

revoke execute on function public.enviar_partida(text, text, text, text) from public, authenticated;
grant execute on function public.enviar_partida(text, text, text, text) to anon;
