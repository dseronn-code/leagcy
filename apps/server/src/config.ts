export const config = {
  port: Number(process.env.PORT ?? 8080),
  jwtSecret: process.env.JWT_SECRET ?? 'dev-secret-change-me',
  databaseUrl: process.env.DATABASE_URL,
  jamendoClientId: process.env.JAMENDO_CLIENT_ID,
  subsonic: process.env.SUBSONIC_URL
    ? { url: process.env.SUBSONIC_URL, user: process.env.SUBSONIC_USER ?? '', password: process.env.SUBSONIC_PASSWORD ?? '' }
    : undefined,
};
