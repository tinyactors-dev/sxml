<!-- ANCHOR: html -->
<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    exclude-result-prefixes="xs">
  <xsl:output method="html" indent="yes"/>

  <!-- "show" writes the editorial notes into the page. -->
  <xsl:param name="notes" select="'hide'"/>

  <xsl:template match="/">
    <html>
      <head>
        <title>{rfc/front/title}</title>
      </head>
      <body>
        <h1>{rfc/front/title}</h1>
        <!-- The same sections, processed twice in two modes. -->
        <xsl:apply-templates select="." mode="toc"/>
        <xsl:apply-templates select="rfc/middle/section"/>
      </body>
    </html>
  </xsl:template>

  <!-- The page: one rule for each kind of element. -->
  <xsl:template match="section">
    <section id="{@anchor}">
      <h2>{name}</h2>
      <xsl:apply-templates select="* except name"/>
    </section>
  </xsl:template>

  <xsl:template match="t">
    <p><xsl:apply-templates/></p>
  </xsl:template>

  <xsl:template match="t[@anchor]" priority="1">
    <p id="{@anchor}" class="requirement"><xsl:apply-templates/></p>
  </xsl:template>

  <xsl:template match="xref">
    <a href="#{@target}">{if (node()) then . else @target}</a>
  </xsl:template>

  <xsl:template match="bcp14">
    <span>
      <xsl:attribute name="class">
        <xsl:call-template name="strength">
          <xsl:with-param name="keyword" select="normalize-space()"/>
        </xsl:call-template>
      </xsl:attribute>
      <xsl:value-of select="."/>
    </span>
  </xsl:template>

  <xsl:template match="cref">
    <xsl:if test="$notes = 'show'">
      <span class="note">[{@source}: {normalize-space()}]</span>
    </xsl:if>
  </xsl:template>

  <xsl:template match="dl | dd">
    <xsl:copy><xsl:apply-templates/></xsl:copy>
  </xsl:template>

  <xsl:template match="dt">
    <dt id="{@anchor}"><xsl:apply-templates/></dt>
  </xsl:template>

  <xsl:template match="sourcecode">
    <pre>{.}</pre>
  </xsl:template>

  <!-- A named template, called with a typed parameter. -->
  <xsl:template name="strength">
    <xsl:param name="keyword" as="xs:string"/>
    <xsl:choose>
      <xsl:when test="$keyword = ('MUST', 'MUST NOT', 'REQUIRED', 'SHALL', 'SHALL NOT')">bcp14 absolute</xsl:when>
      <xsl:when test="starts-with($keyword, 'SHOULD') or contains($keyword, 'RECOMMENDED')">bcp14 recommended</xsl:when>
      <xsl:otherwise>bcp14 optional</xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <!-- The table of contents: the mode "toc". -->
  <xsl:template match="/" mode="toc">
    <ol class="toc">
      <xsl:apply-templates select="rfc/middle/section" mode="toc"/>
    </ol>
  </xsl:template>

  <xsl:template match="section" mode="toc">
    <li>
      <a href="#{@anchor}">{name}</a>
      <xsl:if test="section">
        <ol>
          <xsl:apply-templates select="section" mode="toc"/>
        </ol>
      </xsl:if>
    </li>
  </xsl:template>

  <!-- ANCHOR_END: html -->
  <!-- ANCHOR: text -->
  <!-- Plain text in the style of an RFC: the mode "text". -->
  <xsl:template match="/" mode="text">
    <xsl:apply-templates select="rfc/front/title, rfc/middle/section" mode="text"/>
  </xsl:template>

  <xsl:template match="title" mode="text">
    <xsl:text>{.}&#10;&#10;</xsl:text>
  </xsl:template>

  <xsl:template match="section" mode="text">
    <xsl:variable name="number" select="
        string-join(ancestor-or-self::section ! (count(preceding-sibling::section) + 1), '.')"/>
    <xsl:text>{$number}.  {name}&#10;&#10;</xsl:text>
    <xsl:apply-templates select="* except name" mode="text"/>
  </xsl:template>

  <xsl:template match="t | dd" mode="text">
    <xsl:variable name="text">
      <xsl:apply-templates mode="text"/>
    </xsl:variable>
    <xsl:variable name="indent" select="if (self::dd) then '      ' else '   '"/>
    <!-- Lines of at most 72 characters, indented. -->
    <xsl:value-of select="replace(normalize-space($text),
                                  '(.{1,' || 72 - string-length($indent) || '})( |$)',
                                  $indent || '$1&#10;')"/>
    <xsl:text>&#10;</xsl:text>
  </xsl:template>

  <xsl:template match="dl" mode="text">
    <xsl:apply-templates select="*" mode="text"/>
  </xsl:template>

  <xsl:template match="dt" mode="text">
    <xsl:text>   {.}&#10;</xsl:text>
  </xsl:template>

  <xsl:template match="sourcecode" mode="text">
    <xsl:value-of select="tokenize(., '&#10;')[normalize-space()] ! ('     ' || . || '&#10;')"
                  separator=""/>
    <xsl:text>&#10;</xsl:text>
  </xsl:template>

  <xsl:template match="xref[empty(node())]" mode="text">[{@target}]</xsl:template>

  <xsl:template match="cref" mode="text"/>
  <!-- ANCHOR_END: text -->
</xsl:stylesheet>
