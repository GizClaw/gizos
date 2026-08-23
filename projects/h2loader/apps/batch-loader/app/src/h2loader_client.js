let clientPromise;
let liveClient;
let lifecycleClosed = false;

export function getLoader() {
  if (lifecycleClosed) {
    return Promise.reject(new DOMException("H2Loader page lifecycle ended", "InvalidStateError"));
  }
  if (!clientPromise) {
    const sdkUrl = new URL("./sdk/h2loader.js", document.baseURI).href;
    const creation = import(/* @vite-ignore */ sdkUrl)
      .then(({ createH2Loader }) => createH2Loader());
    clientPromise = creation.then((client) => {
      if (lifecycleClosed) {
        void client.close?.().catch(() => {});
        throw new DOMException("H2Loader page lifecycle ended", "InvalidStateError");
      }
      liveClient = client;
      return client;
    }).catch((error) => {
      clientPromise = undefined;
      throw error;
    });
  }
  return clientPromise;
}

export function closeLoader() {
  lifecycleClosed = true;
  const current = clientPromise;
  clientPromise = undefined;
  if (liveClient) {
    const loader = liveClient;
    liveClient = undefined;
    return Promise.resolve(loader.close?.());
  }
  return current?.then((loader) => loader.close?.(), () => {}) ?? Promise.resolve();
}

export function resetLoaderForTest() {
  clientPromise = undefined;
  liveClient = undefined;
  lifecycleClosed = false;
}
