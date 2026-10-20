export const config = {
  port: Number(process.env.PORT ?? 8080),
  jwtSecret: process.env.JWT_SECRET ?? 'dev-secret-change-me',
  /** Habilita /admin/* (relatórios de royalties, ajuste manual de assinatura). */
  adminToken: process.env.ADMIN_TOKEN,
  /** Dias de teste grátis do catálogo licenciado para contas novas (0 = sem teste). */
  trialDays: Number(process.env.TRIAL_DAYS ?? 0),
  databaseUrl: process.env.DATABASE_URL,
  jamendoClientId: process.env.JAMENDO_CLIENT_ID,
  subsonic: process.env.SUBSONIC_URL
    ? { url: process.env.SUBSONIC_URL, user: process.env.SUBSONIC_USER ?? '', password: process.env.SUBSONIC_PASSWORD ?? '' }
    : undefined,
};
