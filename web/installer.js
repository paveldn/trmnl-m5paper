const webInstaller = document.querySelector("esp-web-install-button");
const firmwareVersion = document.querySelector("#firmware-version");
const releasePicker = document.querySelector(".release-picker");
const releaseSelect = document.querySelector("#firmware-release");
const includeBeta = document.querySelector("#include-beta");

let releases = [];

function selectRelease(tag) {
  const release = releases.find((candidate) => candidate.tag === tag);
  if (!release) {
    return;
  }

  webInstaller.setAttribute("manifest", release.manifest);
  firmwareVersion.textContent = `v${release.version.replace(/^v/, "")}`;
  firmwareVersion.dataset.state = "ready";
}

function renderReleaseOptions(preferredTag) {
  const visibleReleases = releases.filter(
    (release) => includeBeta.checked || !release.prerelease,
  );
  const selectedTag = visibleReleases.some(
    (release) => release.tag === preferredTag,
  )
    ? preferredTag
    : visibleReleases[0]?.tag;

  releaseSelect.replaceChildren(
    ...visibleReleases.map((release) => {
      const option = document.createElement("option");
      option.value = release.tag;
      option.textContent = `${release.tag}${release.prerelease ? " (beta)" : ""}`;
      option.selected = release.tag === selectedTag;
      return option;
    }),
  );

  if (selectedTag) {
    selectRelease(selectedTag);
  }
}

async function loadFirmwareVersion() {
  try {
    const catalogResponse = await fetch("releases.json", { cache: "no-store" });
    if (catalogResponse.ok) {
      const catalog = await catalogResponse.json();
      if (!Array.isArray(catalog.releases) || catalog.releases.length === 0) {
        throw new Error("Release catalog is empty");
      }

      releases = catalog.releases;
      renderReleaseOptions(catalog.default);
      releasePicker.disabled = false;
      return;
    }

    // Local development builds contain one root manifest and no release catalog.
    const manifestUrl = webInstaller.getAttribute("manifest");
    const response = await fetch(manifestUrl, { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`Manifest request failed with status ${response.status}`);
    }

    const manifest = await response.json();
    if (typeof manifest.version !== "string" || !manifest.version.trim()) {
      throw new Error("Manifest does not contain a valid version");
    }

    firmwareVersion.textContent = `v${manifest.version.trim().replace(/^v/, "")}`;
    firmwareVersion.dataset.state = "ready";
  } catch {
    firmwareVersion.textContent = "Version unavailable";
    firmwareVersion.dataset.state = "error";
  }
}

releaseSelect.addEventListener("change", () => selectRelease(releaseSelect.value));
includeBeta.addEventListener("change", () =>
  renderReleaseOptions(releaseSelect.value),
);

loadFirmwareVersion();
