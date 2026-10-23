# Baixa os modelos de exemplo oficiais da Khronos (glTF-Sample-Assets) para engine/assets/.
# Uso (de dentro de engine/):  cmake -P tools/fetch_assets.cmake
# Os arquivos não vão para o git (ver .gitignore).

set(BASE "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models")
set(MODELS Sponza DamagedHelmet Fox CesiumMan)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../assets" ABSOLUTE)

function(download url dest)
  if(EXISTS "${dest}")
    return()
  endif()
  message(STATUS "  baixando ${url}")
  file(DOWNLOAD "${url}" "${dest}" STATUS st TLS_VERIFY ON)
  list(GET st 0 code)
  if(NOT code EQUAL 0)
    file(REMOVE "${dest}")
    message(FATAL_ERROR "Falha ao baixar ${url}: ${st}")
  endif()
endfunction()

foreach(model ${MODELS})
  message(STATUS "${model}")
  set(dir "${ROOT}/${model}")
  set(gltf "${dir}/${model}.gltf")
  download("${BASE}/${model}/glTF/${model}.gltf" "${gltf}")
  # Baixa todos os arquivos referenciados por "uri" (buffers e texturas).
  file(READ "${gltf}" json)
  string(REGEX MATCHALL "\"uri\"[ \t\r\n]*:[ \t\r\n]*\"[^\"]+\"" uris "${json}")
  foreach(entry ${uris})
    string(REGEX REPLACE ".*\"([^\"]+)\"$" "\\1" uri "${entry}")
    if(uri MATCHES "^data:")
      continue()
    endif()
    download("${BASE}/${model}/glTF/${uri}" "${dir}/${uri}")
  endforeach()
endforeach()
message(STATUS "Assets prontos em ${ROOT}")
