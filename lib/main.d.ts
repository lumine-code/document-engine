export interface Point {
  row: number
  column: number
}

export interface DisplayViewOptions {
  wrapColumn?: number
  tabLength?: number
  softWrapHangingIndent?: number
  atomicSoftTabs?: boolean
  wrapBoundaryMode?: 'none' | 'word' | 'standard'
  foldCharacter?: string
  invisibles?: {tab?: string; space?: string; eol?: string; cr?: string}
  builtInScopeIds?: Record<number, number>
  characterWidthProfile?: {
    default?: number
    doubleWidth?: number
    halfWidth?: number
    korean?: number
  }
}

export interface DocumentSessionOptions {
  languageId?: string
  wasmPath?: string
  workerDelayMs?: number
  maxSyntaxUtf16Length?: number
  /** Test/compatibility switch that exercises the base V1 chunk fallback. */
  disableSnapshotLineIndex?: boolean
}

export interface RevisionResult {
  accepted: boolean
  bufferRevision: number
  syntaxRevision: number
  checksum: string
  syntaxParsed: boolean
  syntaxIncremental: boolean
  syntaxDisabledReason:
    | ''
    | 'input-too-large'
    | 'query-error'
    | 'grammar-error'
    | 'configuration-error'
    | 'parse-error'
  syntaxErrorCode: string
  syntaxChecksum: string
  syntaxRootType: string
  resolvedLanguageName: string
  syntaxRootHasError: boolean
  syntaxNodeCount: number
  grammarLoadMilliseconds: number
  grammarCacheHit: boolean
  parseMilliseconds: number
  queryCaptureCount: number
  queryCompileMilliseconds: number
  queryExecuteMilliseconds: number
}

export interface QueryOperand {
  type: 'capture' | 'string'
  value: string
}

export interface QueryPatternMetadata {
  set: Record<string, string | null>
  asserted: Record<string, string | null>
  refuted: Record<string, string | null>
  predicates: Array<{operator: string; operands: QueryOperand[]}>
  unresolvedPredicates: Array<{operator: string; operands: QueryOperand[]}>
  scopeRegexComplete: boolean
}

export interface QueryCaptureResult {
  queryType:
    | 'highlightsQuery'
    | 'foldsQuery'
    | 'indentsQuery'
    | 'localsQuery'
    | 'tagsQuery'
    | 'injectionsQuery'
  bufferRevision: number
  syntaxRevision: number
  languageGeneration: number
  captureStride: 9
  /** [nameId, patternIndex, propertySetId, startRow, startColumn, endRow, endColumn, startIndex, endIndex] */
  captures: Uint32Array
  captureNames: string[]
  propertySets: QueryPatternMetadata[]
  layers: QueryLayerResult[]
  /** One entry per capture; indexes `layers`. */
  captureLayerIndices: Uint32Array
  captureDepths: Uint32Array
  captureOrders: Uint32Array
  /** Bit 0 marks a synthesized injection language-scope capture. */
  captureFlags: Uint32Array
  captureNodeHandles: Uint32Array
  highlightRangeStride: 5
  /** [nameId, startUtf16, endUtf16, order, depth] */
  highlightRanges: Uint32Array
  /** One entry per highlight range; indexes `highlightGrammarIds` and full-result `layers`. */
  highlightLayerIndices: Uint32Array
  /** Compact grammar-id lookup for `highlightLayerIndices`. */
  highlightGrammarIds: string[]
  highlightFlags: Uint32Array
  highlightNodeHandles: Uint32Array
  rawCaptureCount: number
  acceptedCaptureCount: number
  resolutionComplete: boolean
  scopeCapturesTested: number
  scopeCapturesRejected: number
  scopeCapturesAdjusted: number
  didExceedMatchLimit: boolean
}

export interface QueryLayerResult {
  layerId: number
  depth: number
  grammarId: string
  coverShallowerScopes: boolean
  rangeStride: 6
  /** [startRow, startColumn, endRow, endColumn, startUtf16, endUtf16] */
  ranges: Uint32Array
  rangeScopes: string[][]
}

export interface QueryResolutionOptions {
  /** Exact UTF-16 column of the query range start; defaults to zero. */
  startColumn?: number
  /** Exact UTF-16 column of the query range end; defaults to zero for a bounded end row. */
  endColumn?: number
  resolveScopes?: boolean
  interpolateNames?: boolean
  injectionDepth?: number
  includeLanguageScopes?: boolean
  /** Omit detailed capture, pattern and layer metadata for display highlighting. */
  compactHighlights?: boolean
  scopeConfig?: Record<string, null | boolean | number | string>
  scopeConfigsByLanguage?: Record<
    string,
    Record<string, null | boolean | number | string>
  >
  /** Packed exact [startIndex, endIndex] pairs; optional future lexical-local context. */
  localRanges?: Uint32Array
}

export interface InjectionRevisionTags {
  bufferRevision: number
  syntaxRevision: number
  languageGeneration: number
}

export interface InjectionRegistrationManifest {
  injectionPointGeneration: number
  injectionRescanWave?: number
  grammars: Array<{
    grammarId: string
    types: string[]
    registrations: Array<{id: number; type: string}>
  }>
}

export interface InjectionCandidateBatch extends InjectionRevisionTags {
  accepted: boolean
  reason?: string
  requestId: number
  injectionPointGeneration: number
  candidateStride: 4 | 5
  /** Stride 4: [candidateId, grammarIdIndex, kindIndex, nodeHandle]. Stride 5 adds depth. */
  candidates: Uint32Array
  grammarIds: string[]
  kinds: string[]
  queryLayerCount: number
  candidateQueueMilliseconds: number
  candidateScanMilliseconds: number
  candidatePackMilliseconds: number
  unresolvedQueryLanguages?: string[]
  sourceTokenFingerprint?: string
  scannedSourceCount?: number
}

export interface InjectionNode {
  /** Snapshot-backed properties remain readable after the session advances. */
  readonly id: number
  readonly type: string
  readonly text: string
  readonly startIndex: number
  readonly endIndex: number
  readonly startPosition: Point
  readonly endPosition: Point
  readonly range: {start: Point; end: Point}
  readonly parent: InjectionNode | null
  readonly children: InjectionNode[]
  readonly namedChildren: InjectionNode[]
  readonly childCount: number
  readonly namedChildCount: number
  readonly firstChild: InjectionNode | null
  readonly lastChild: InjectionNode | null
  readonly firstNamedChild: InjectionNode | null
  readonly lastNamedChild: InjectionNode | null
  readonly previousSibling: InjectionNode | null
  readonly nextSibling: InjectionNode | null
  readonly previousNamedSibling: InjectionNode | null
  readonly nextNamedSibling: InjectionNode | null
  readonly isNamed: boolean
  readonly isMissing: boolean
  readonly isError: boolean
  readonly hasError: boolean
  /** True only while this node's immutable tree is the session's current syntax revision. */
  isCurrent(): boolean
  /** Returns this node or throws ERR_STALE_INJECTION_NODE. */
  assertCurrent(): InjectionNode
  child(index: number): InjectionNode | null
  namedChild(index: number): InjectionNode | null
  childForFieldName(name: string): InjectionNode | null
  descendantsOfType(type: string | string[]): InjectionNode[]
}

export interface SyntaxNodeSearchCandidate {
  node: InjectionNode
  grammarId: string
  depth: number
  layerId: number
}

export interface SyntaxNodeSearchResult extends InjectionRevisionTags {
  current: boolean
  node: InjectionNode | null
  grammarId: string | null
  depth: number | null
  layerId: number | null
  candidates: SyntaxNodeSearchCandidate[]
}

export interface DocumentSessionDiagnostics {
  bufferRevision: number
  syntaxRevision: number
  languageGeneration: number
  activeJobs: number
  pendingJobs: number
  activeInjectionJobs: number
  injectionActiveRequests: number
  injectionCandidateCount: number
  injectionLayerCount: number
  dynamicInjectionLayerCount: number
  queryInjectionLayerCount: number
  injectionRangeCount: number
  parsedInjectionLayerCount: number
  failedInjectionLayerCount: number
  maximumInjectionDepth: number
  injectionStaleRequests: number
  injectionAbortedRequests: number
  injectionPublishedGeneration: number
  injectionTopologyGeneration: number
  injectionReusedLayers: number
  injectionProjectedRanges: number
  injectionChildIncrementalParses: number
  injectionChildFullParses: number
  injectionChildTreeEditReuses: number
  injectionChildTreeEditFallbacks: number
  injectionReuseFallbacks: number
  injectionChildBackendCount: number
  syntaxInputTooLarge: number
  syntaxFailOpenCount: number
  [name: string]: number | string | boolean
}

export interface RenderLine {
  id: number
  lineText: string
  tags: Int32Array
  softWrapIndent: number
}

export interface RenderPlan {
  bufferRevision: number
  syntaxRevision: number
  displayRevision: number
  foldGeneration: number
  highlightGeneration: number
  highlightCoverageComplete: boolean
  /** Visible viewport's buffer-row envelope; hidden folded rows inside it need not be indexed. */
  highlightCoverageStartRow: number
  highlightCoverageEndRow: number
  indexedBufferRowCount: number
  lines: RenderLine[]
}

/** @internal Compact RenderPlan transport used by Lumine's native display path. */
export interface PackedRenderPlan {
  bufferRevision: number
  syntaxRevision: number
  displayRevision: number
  foldGeneration: number
  highlightGeneration: number
  highlightCoverageComplete: boolean
  /** Visible viewport's buffer-row envelope; hidden folded rows inside it need not be indexed. */
  highlightCoverageStartRow: number
  highlightCoverageEndRow: number
  indexedBufferRowCount: number
  /** All viewport line text concatenated as UTF-16. */
  text: string
  /** Exact safe-integer RenderLine ids; Float64 avoids uint32 truncation. */
  lineIds: Float64Array
  lineDescriptorStride: 5
  /** [textStart, textLength, tagsStart, tagsLength, softWrapIndentOrUint32Max]. */
  lineDescriptors: Uint32Array
  /** Concatenated tags for every line. */
  tags: Int32Array
}

export interface DisplayViewDiagnostics {
  displayRevision: number
  foldGeneration: number
  targetBufferRevision: number
  cachedBufferRevision: number
  activeFoldCount: number
  highlightBufferRevision: number
  highlightSyntaxRevision: number
  acceptedEditCount: number
  foldDeltaCount: number
  foldDeltaMilliseconds: number
  foldDeltaMaximumMilliseconds: number
  foldResetCount: number
  foldResetMilliseconds: number
  indexRebuildCount: number
  indexRebuildMilliseconds: number
  indexIncrementalUpdateCount: number
  indexInPlaceUpdateCount: number
  indexIncrementalFallbackCount: number
  indexIncrementalUpdateMilliseconds: number
  indexIncrementalRowsRebuilt: number
  indexIncrementalRowsReused: number
  indexIncrementalLayoutUnitsScanned: number
  renderPlanCount: number
  renderPlanMilliseconds: number
  screenRowCount: number
  displaySpanCount: number
  retainedBytes: number
  sourceUtf16Length: number
  layoutUnitsScanned: number
  peakLogicalSegments: number
  peakRowSpans: number
}

export interface IndexedDisplaySummary {
  screenLineCount: number
  rightmostScreenPosition: Point
}

export interface FoldDeltas {
  upserts?: Uint32Array
  removals?: Uint32Array
  /** Packed stride 6: [startRow, startColumn, oldExtentRow, oldExtentColumn, newExtentRow, newExtentColumn]. */
  splices?: Uint32Array
}

export declare class DisplayView {
  applyFoldDeltas(
    bufferRevision: number,
    fromGeneration: number,
    toGeneration: number,
    operations: FoldDeltas,
  ): void
  replaceFolds(
    bufferRevision: number,
    generation: number,
    packedRanges: Uint32Array,
  ): void
  /** Packed stride 5: [scopeId, startUtf16, endUtf16, order, depth]. */
  replaceHighlightRanges(
    bufferRevision: number,
    syntaxRevision: number,
    packedRanges: Uint32Array,
  ): void
  bufferToScreen(
    point: Point,
    clipDirection?: 'backward' | 'forward' | 'closest',
  ): Point
  bufferToScreen(
    points: Uint32Array,
    clipDirection?: 'backward' | 'forward' | 'closest',
  ): Uint32Array
  screenToBuffer(
    point: Point,
    clipDirection?: 'backward' | 'forward' | 'closest',
  ): Point
  screenToBuffer(
    points: Uint32Array,
    clipDirection?: 'backward' | 'forward' | 'closest',
  ): Uint32Array
  projectBufferRanges(ranges: Uint32Array): Uint32Array
  getScreenLineCount(): number
  lineLengthForScreenRow(screenRow: number): number | undefined
  getRightmostScreenPosition(): Point
  getIndexedSummary(bufferRowCount: number): IndexedDisplaySummary
  bufferRowsForScreenRows(startRow: number, endRow: number): Uint32Array
  /** @internal Native highlight shards intersecting visible source/fold spans. */
  _highlightShardStartsForScreenRows(
    startRow: number,
    endRow: number,
  ): Uint32Array
  /** Packed stride 5: [startRow, startColumn, endRow, endColumn, screenColumnOrUint32Max]. */
  translateScreenColumnBlock(
    startRow: number,
    endRow: number,
    startColumn: number,
    endColumn: number,
  ): Uint32Array
  buildRenderPlan(startScreenRow: number, endScreenRow: number): RenderPlan
  /** @internal Packed transport for Lumine; use buildRenderPlan for compatibility. */
  buildRenderPlanPacked(
    startScreenRow: number,
    endScreenRow: number,
  ): PackedRenderPlan
  getDiagnostics(): DisplayViewDiagnostics
  destroy(): void
}

export declare class DocumentSession {
  constructor(options?: DocumentSessionOptions)
  applyRevision(
    snapshot: object,
    edits: Uint32Array | null,
    bufferRevision: number,
    foldUpdates?: unknown,
  ): Promise<RevisionResult>
  createDisplayView(options?: DisplayViewOptions): DisplayView
  setLanguage(
    descriptor: null | {
      languageId: string
      runtime: string
      wasmPath: string
      languageName?: string
      languageSegment?: string
      queryPaths?: Record<string, string[]>
    },
  ): void
  configureSyntax(options: {
    languageId: string
    wasmPath: string
    languageName?: string
    languageSegment?: string
    queries?: Record<string, string>
  }): void
  getQueryCaptures(
    queryType: string,
    startRow?: number,
    endRow?: number,
    options?: QueryResolutionOptions,
  ): QueryCaptureResult
  getQueryRequirements(queryType: string): {
    queryType: QueryCaptureResult['queryType']
    scopeConfigKeys: string[]
    scopeConfigKeysByLanguage: Record<string, string[]>
  }
  requestHighlightCoverage(
    startRow: number,
    endRow: number,
    options: {
      contextGeneration: number
      scopeConfig?: Record<string, null | boolean | number | string>
      scopeConfigsByLanguage?: Record<
        string,
        Record<string, null | boolean | number | string>
      >
      /** Optional sorted native 128-row shards for a folded viewport. */
      shardStarts?: Uint32Array
    },
  ): Promise<{
    accepted: boolean
    published: boolean
    needsCommit: boolean
    fallbackSync: boolean
    permanentIncomplete: boolean
    reason?: string
    requestId: number
    bufferRevision: number
    syntaxRevision: number
    languageGeneration: number
    injectionGeneration: number
    contextGeneration: number
    coverageStartRow: number
    coverageEndRow: number
    highlightGeneration: number
    captureNames: string[]
    captureGrammarIds: string[]
    queueMilliseconds?: number
    queryMilliseconds?: number
  }>
  commitHighlightCoverage(
    requestId: number,
    scopeIds: Uint32Array,
  ): {
    accepted: boolean
    published: boolean
    needsCommit: false
    fallbackSync: boolean
    permanentIncomplete: boolean
    reason?: string
    requestId: number
    bufferRevision: number
    syntaxRevision: number
    languageGeneration: number
    injectionGeneration: number
    contextGeneration: number
    coverageStartRow: number
    coverageEndRow: number
    highlightGeneration: number
  }
  abortHighlightCoverage(requestId: number): boolean
  invalidateHighlightIndex(): number
  /** @internal Forces the compatibility display path for this session. */
  useSynchronousHighlights(): void
  getInjectionCandidates(
    request: InjectionRevisionTags & InjectionRegistrationManifest,
  ): Promise<InjectionCandidateBatch>
  resolveInjectionNode(
    candidate: {requestId: number; candidateId: number; nodeHandle?: number},
    tags: InjectionRevisionTags,
  ): InjectionNode
  resolveQueryNode(
    identity: {layerId: number; nodeHandle: number},
    tags: InjectionRevisionTags,
  ): InjectionNode
  getSyntaxNodeAtPosition(
    point: Point | [number, number],
  ): SyntaxNodeSearchResult
  getSyntaxNodeContainingRange(
    range: {start: Point; end: Point} | [[number, number], [number, number]],
  ): SyntaxNodeSearchResult
  applyInjectionResultBatch(
    batch: InjectionRevisionTags & {
      requestId: number
      injectionPointGeneration: number
      batchIndex: number
      isFinal: boolean
      results: unknown[]
    },
  ): Promise<Record<string, unknown>> | Record<string, unknown>
  applyInjectionResults(
    batch: InjectionRevisionTags & {
      requestId: number
      injectionPointGeneration: number
      results: unknown[]
    },
  ): Promise<Record<string, unknown>> | Record<string, unknown>
  applyInjectionLanguageScopes(
    batch: InjectionRevisionTags & {
      requestId: number
      injectionPointGeneration: number
      results: unknown[]
    },
  ): Promise<Record<string, unknown>> | Record<string, unknown>
  applyQueryLanguageDescriptors(
    batch: InjectionRevisionTags & {
      requestId: number
      injectionPointGeneration: number
      descriptors: Array<{
        alias: string
        grammar: null | {
          languageId: string
          runtime: string
          wasmPath: string
          languageName?: string
          languageSegment?: string
          queryPaths?: Record<string, string[]>
        }
      }>
    },
  ): Promise<Record<string, unknown>> | Record<string, unknown>
  abortInjectionRequest(request: {
    requestId: number
    reason?: string
  }): Record<string, unknown>
  getDiagnostics(): DocumentSessionDiagnostics
  drain(): Promise<void>
  destroy(): Promise<void>
}

export declare const capabilities: Readonly<{
  nativeSyntaxWasm: boolean
  nativeQueries: boolean
  nativeGrammarCache: boolean
  nativeQueryCache: boolean
  syntaxBackend: string
  treeSitterVersion: string
  wasmtimeVersion: string
  maxSyntaxUtf16Length: number
  grammarCacheCapacity: number
  queryCacheCapacity: number
  snapshotLeaseAbi: number
  nativeDisplayIndex: boolean
  nativeDisplayParity: boolean
  nativeAsyncHighlights: boolean
  nativeDynamicInjections: boolean
  nativeInjectionChildParsing: boolean
  nativeLayeredQueries: boolean
  nativeQueryLanguageResolution: boolean
  nativeSyntaxNodeApi: boolean
}>
