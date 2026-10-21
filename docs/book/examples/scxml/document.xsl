<!-- An XHTML page about a Latch machine, from its annotations and from
     the schema of its data, latch-data.xsd. -->
<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns="http://www.w3.org/1999/xhtml"
    xmlns:html="http://www.w3.org/1999/xhtml"
    xmlns:sc="http://www.w3.org/2005/07/scxml"
    xmlns:latch="urn:example:latch"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:doc="urn:example:document"
    exclude-result-prefixes="html sc latch xs doc">
  <xsl:output method="xhtml" html-version="5" indent="yes"/>

  <!-- Where the published draft is, for links to its requirements. -->
  <xsl:param name="draft" select="'latch.html'"/>

  <xsl:variable name="schema" select="doc('latch-data.xsd')/xs:schema"/>

  <xsl:template match="/sc:scxml">
    <html>
      <head>
        <title>The {@name} machine</title>
      </head>
      <body>
        <h1>The {@name} machine</h1>
        <p>It starts in the state <a href="#{@initial}">{@initial}</a>.
          <xsl:apply-templates select="@latch:implements"/></p>
        <h2>States</h2>
        <xsl:apply-templates select="sc:state"/>
        <h2>Transitions</h2>
        <table>
          <tr><th>From</th><th>Event</th><th>Condition</th><th>To</th></tr>
          <xsl:apply-templates select="sc:state/sc:transition"/>
        </table>
        <h2>Events</h2>
        <dl>
          <xsl:apply-templates select="latch:events/latch:event"/>
        </dl>
        <h2>Data</h2>
        <table>
          <tr><th>Item</th><th>Initial value</th><th>Type</th></tr>
          <xsl:apply-templates select="sc:datamodel/sc:data"/>
        </table>
      </body>
    </html>
  </xsl:template>

  <!-- A state: its own XHTML, copied without the machine's namespaces. -->
  <xsl:template match="sc:state">
    <section id="{@id}">
      <h3>{@id}</h3>
      <xsl:copy-of select="html:div/node()" copy-namespaces="no"/>
      <xsl:apply-templates select="@latch:implements"/>
    </section>
  </xsl:template>

  <xsl:template match="sc:transition">
    <tr>
      <td><a href="#{../@id}">{../@id}</a></td>
      <td>{@event}</td>
      <td><xsl:if test="@cond"><code>{@cond}</code></xsl:if></td>
      <td>
        <xsl:choose>
          <xsl:when test="@target"><a href="#{@target}">{@target}</a></xsl:when>
          <xsl:otherwise>(stays)</xsl:otherwise>
        </xsl:choose>
      </td>
    </tr>
  </xsl:template>

  <!-- An event: its XHTML description, and what its payload carries. -->
  <xsl:template match="latch:event">
    <dt id="event-{@name}">{@name}</dt>
    <dd>
      <xsl:copy-of select="html:*" copy-namespaces="no"/>
      <xsl:apply-templates select="@latch:payload"/>
    </dd>
  </xsl:template>

  <xsl:template match="@latch:payload">
    <xsl:variable name="name" select="local-name-from-QName(resolve-QName(., ..))"/>
    <p>It carries a <code>{.}</code>:</p>
    <ul>
      <xsl:for-each select="$schema/xs:element[@name = $name]//xs:element">
        <li><code>{@name}</code>{if (@minOccurs = '0') then ' (optional)' else ''},
          {doc:describe(@type)}</li>
      </xsl:for-each>
    </ul>
  </xsl:template>

  <xsl:template match="sc:data">
    <tr>
      <td>{@id}</td>
      <td>{@expr}</td>
      <td><code>{@latch:type}</code>: {doc:describe(@latch:type)}</td>
    </tr>
  </xsl:template>

  <!-- Links to the requirements of the draft. -->
  <xsl:template match="@latch:implements">
    <span class="implements">Implements
      <xsl:for-each select="tokenize(.)">
        <a href="{$draft}#{.}">{.}</a>
        <xsl:if test="position() ne last()">, </xsl:if>
      </xsl:for-each>.</span>
  </xsl:template>

  <!-- A type of latch-data.xsd in words: its documentation if it has
       one, else its base type and facets. -->
  <xsl:function name="doc:describe" as="xs:string">
    <xsl:param name="type" as="attribute()"/>
    <xsl:variable name="name" select="resolve-QName($type, $type/..)"/>
    <xsl:variable name="definition" select="
      $schema/xs:simpleType[@name = local-name-from-QName($name)]
        [namespace-uri-from-QName($name) eq string($schema/@targetNamespace)]"/>
    <xsl:choose>
      <xsl:when test="empty($definition)">{
        if (namespace-uri-from-QName($name) eq 'http://www.w3.org/2001/XMLSchema')
        then 'xs:' || local-name-from-QName($name) else 'an unknown type'}</xsl:when>
      <xsl:when test="$definition/xs:annotation/xs:documentation">{
        normalize-space($definition/xs:annotation/xs:documentation)}</xsl:when>
      <xsl:otherwise>
        <xsl:variable name="restriction" select="$definition/xs:restriction"/>
        <xsl:sequence select="string-join((
          $restriction/@base,
          $restriction/xs:pattern/@value ! `matching {.}`,
          $restriction/xs:minInclusive/@value ! `at least {.}`,
          $restriction/xs:maxInclusive/@value ! `at most {.}`,
          if ($restriction/xs:enumeration)
          then 'one of ' || string-join($restriction/xs:enumeration/@value, ', ')
          else ()
        ), ', ')"/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:function>
</xsl:stylesheet>
