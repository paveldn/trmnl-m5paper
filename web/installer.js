const modelConfirmation = document.querySelector("#model-confirmation");
const installButton = document.querySelector("#install-button");
const webInstaller = document.querySelector("esp-web-install-button");
const firmwareVersion = document.querySelector("#firmware-version");

function updateInstallButton() {
  installButton.disabled = !modelConfirmation.checked;
}

async function loadFirmwareVersion() {
  try {
    const manifestUrl = webInstaller.getAttribute("manifest");
    if (!manifestUrl) {
      throw new Error("Installer does not define a manifest URL");
    }

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

modelConfirmation.addEventListener("change", updateInstallButton);
updateInstallButton();
loadFirmwareVersion();
