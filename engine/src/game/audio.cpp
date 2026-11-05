#include "game/audio.h"

#include <algorithm>
#include <cstdio>
#include <list>
#include <vector>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_GENERATION  // geradores de forma de onda/ruído: não usados
#include <miniaudio.h>

#include "core/log.h"

namespace game {

struct Audio::Impl {
  ma_engine engine{};
  bool ready = false;
  bool offline = false;
  std::vector<std::unique_ptr<ma_sound>> sounds;  // índice = identificador
  std::list<ma_sound> oneShots;                   // tocados uma vez; limpos quando terminam
  std::vector<float> captured;                    // offline: mixagem intercalada L R
  double pendingFrames = 0.0;

  void reapOneShots() {
    for (auto it = oneShots.begin(); it != oneShots.end();) {
      if (ma_sound_at_end(&*it)) {
        ma_sound_uninit(&*it);
        it = oneShots.erase(it);
      } else {
        ++it;
      }
    }
  }
};

namespace {

void applyDesc(ma_sound* s, const Audio::SoundDesc& d) {
  ma_sound_set_volume(s, d.volume);
  ma_sound_set_looping(s, d.loop ? MA_TRUE : MA_FALSE);
  ma_sound_set_spatialization_enabled(s, d.spatial ? MA_TRUE : MA_FALSE);
  ma_sound_set_attenuation_model(s, ma_attenuation_model_inverse);
  ma_sound_set_min_distance(s, d.minDistance);
  ma_sound_set_max_distance(s, d.maxDistance);
  ma_sound_set_rolloff(s, d.rolloff);
}

}  // namespace

Audio::Audio(bool offline) : p_(std::make_unique<Impl>()) {
  p_->offline = offline;
  ma_engine_config cfg = ma_engine_config_init();
  cfg.channels = 2;
  cfg.sampleRate = kSampleRate;
  cfg.noDevice = offline ? MA_TRUE : MA_FALSE;
  ma_result r = ma_engine_init(&cfg, &p_->engine);
  if (r != MA_SUCCESS && !offline) {  // sem placa de som: continua mudo (a mixagem só não sai)
    LOG_WARN("Áudio: sem dispositivo de som (%s); seguindo sem som", ma_result_description(r));
    cfg.noDevice = MA_TRUE;
    p_->offline = true;
    r = ma_engine_init(&cfg, &p_->engine);
  }
  p_->ready = r == MA_SUCCESS;
  if (!p_->ready) LOG_ERROR("Áudio: ma_engine_init falhou (%s)", ma_result_description(r));
}

Audio::~Audio() {
  if (!p_->ready) return;
  for (auto& s : p_->oneShots) ma_sound_uninit(&s);
  for (auto& s : p_->sounds)
    if (s) ma_sound_uninit(s.get());
  ma_engine_uninit(&p_->engine);
}

bool Audio::ok() const { return p_->ready; }

uint32_t Audio::create(const std::string& path, const SoundDesc& desc, glm::vec3 position) {
  if (!p_->ready) return ~0u;
  auto s = std::make_unique<ma_sound>();
  // Decodificado inteiro na memória: sons curtos e de laço sem travar o áudio lendo o disco.
  const ma_result r = ma_sound_init_from_file(&p_->engine, path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, s.get());
  if (r != MA_SUCCESS) {
    LOG_ERROR("Áudio: não abri %s (%s)", path.c_str(), ma_result_description(r));
    return ~0u;
  }
  applyDesc(s.get(), desc);
  ma_sound_set_position(s.get(), position.x, position.y, position.z);
  p_->sounds.push_back(std::move(s));
  return uint32_t(p_->sounds.size() - 1);
}

void Audio::play(uint32_t i) {
  if (i < p_->sounds.size() && p_->sounds[i]) ma_sound_start(p_->sounds[i].get());
}
void Audio::stop(uint32_t i) {
  if (i < p_->sounds.size() && p_->sounds[i]) ma_sound_stop(p_->sounds[i].get());
}
void Audio::setPosition(uint32_t i, glm::vec3 p, glm::vec3 v) {
  if (i >= p_->sounds.size() || !p_->sounds[i]) return;
  ma_sound_set_position(p_->sounds[i].get(), p.x, p.y, p.z);
  ma_sound_set_velocity(p_->sounds[i].get(), v.x, v.y, v.z);
}

void Audio::playOnce(const std::string& path, glm::vec3 position, float volume) {
  if (!p_->ready) return;
  p_->reapOneShots();
  p_->oneShots.emplace_back();
  ma_sound& s = p_->oneShots.back();
  if (ma_sound_init_from_file(&p_->engine, path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, &s) != MA_SUCCESS) {
    p_->oneShots.pop_back();
    LOG_ERROR("Áudio: não abri %s", path.c_str());
    return;
  }
  SoundDesc d;
  d.volume = volume;
  applyDesc(&s, d);
  ma_sound_set_position(&s, position.x, position.y, position.z);
  ma_sound_start(&s);
}

void Audio::setListener(glm::vec3 p, glm::vec3 f, glm::vec3 v) {
  if (!p_->ready) return;
  ma_engine_listener_set_position(&p_->engine, 0, p.x, p.y, p.z);
  ma_engine_listener_set_direction(&p_->engine, 0, f.x, f.y, f.z);
  ma_engine_listener_set_world_up(&p_->engine, 0, 0.0f, 1.0f, 0.0f);
  ma_engine_listener_set_velocity(&p_->engine, 0, v.x, v.y, v.z);
}

void Audio::mix(float seconds) {
  if (!p_->ready || !p_->offline || seconds <= 0.0f) return;
  p_->pendingFrames += double(seconds) * kSampleRate;
  const ma_uint64 frames = ma_uint64(p_->pendingFrames);
  p_->pendingFrames -= double(frames);
  const size_t start = p_->captured.size();
  p_->captured.resize(start + frames * 2);
  ma_uint64 read = 0;
  ma_engine_read_pcm_frames(&p_->engine, p_->captured.data() + start, frames, &read);
  p_->captured.resize(start + read * 2);
  p_->reapOneShots();
}

bool Audio::writeWav(const std::string& path) const {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t samples = uint32_t(p_->captured.size()), dataBytes = samples * 2;
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f);
  u32(36 + dataBytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16);
  u16(1);  // PCM
  u16(2);
  u32(kSampleRate);
  u32(kSampleRate * 4);
  u16(4);
  u16(16);
  std::fwrite("data", 1, 4, f);
  u32(dataBytes);
  for (float x : p_->captured) {
    const int16_t v = int16_t(std::clamp(x, -1.0f, 1.0f) * 32767.0f);
    std::fwrite(&v, 2, 1, f);
  }
  std::fclose(f);
  LOG_INFO("Áudio gravado em %s (%.2f s)", path.c_str(), double(samples / 2) / kSampleRate);
  return true;
}

}  // namespace game
