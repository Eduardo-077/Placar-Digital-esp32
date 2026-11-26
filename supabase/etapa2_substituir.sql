-- ETAPA 2 - Opção "substituir o jogo em andamento"
-- Colar inteiro no SQL Editor do Supabase (em Database) e clicar em Run.
-- Rodar ANTES de publicar o placar.html novo: o HTML antigo continua
-- funcionando com a função nova (substituir fica false).

-- 1) Nova coluna: true = o placar troca na hora, mesmo com jogo rodando
--    (o "if not exists" deixa rodar o script de novo sem erro)
alter table public.partidas
  add column if not exists substituir boolean not null default false;

-- 2) Troca a função por uma com o parâmetro novo.
--    A antiga precisa sair: com as duas, o Supabase não saberia qual chamar.
drop function if exists public.enviar_partida(text, text, text, text);
drop function if exists public.enviar_partida(text, text, text, text, boolean);

create function public.enviar_partida(
  p_pin text, p_modo text, p_time_a text, p_time_b text,
  p_substituir boolean default false
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

  insert into public.partidas (placar_id, modo, time_a, time_b, substituir)
  values ('placar01', p_modo, trim(p_time_a), trim(p_time_b), p_substituir)
  returning id into novo_id;

  return novo_id;
end;
$$;

revoke execute on function public.enviar_partida(text, text, text, text, boolean) from public, authenticated;
grant execute on function public.enviar_partida(text, text, text, text, boolean) to anon;

-- 3) Avisa a API do Supabase que a tabela e a função mudaram
notify pgrst, 'reload schema';

-- Teste (deve retornar o id novo):
-- set role anon;
-- select public.enviar_partida('1234', 'futebol', 'TESTE A', 'TESTE B', true);
-- reset role;
-- select id, time_a, time_b, substituir from public.partidas order by id desc limit 1;
